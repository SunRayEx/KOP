#include "frame_bridge.h"

#include <cstddef>

namespace kopms {

bool validate_frame_descriptor(const KopmsFrameDescriptor* frame, std::string* error) {
    return validate_frame_descriptor(frame, gpu_mode_from_environment(), error);
}

bool validate_frame_descriptor(const KopmsFrameDescriptor* frame, GpuMode mode,
                               std::string* error) {
    if (!frame) {
        if (error) *error = "帧描述为空";
        return false;
    }
    const uint32_t required = static_cast<uint32_t>(
        offsetof(KopmsFrameDescriptor, release) + sizeof(frame->release));
    if (frame->struct_size < required || frame->version != KOPMS_FRAME_DESCRIPTOR_VERSION) {
        if (error) *error = "帧描述版本或结构体大小不匹配";
        return false;
    }
    if (!frame->retain || !frame->release) {
        if (error) *error = "帧描述缺少 retain/release 生命周期回调";
        return false;
    }
    if (frame->media_type != KOPAW_MEDIA_VIDEO && frame->media_type != KOPAW_MEDIA_AUDIO) {
        if (error) *error = "帧描述媒体类型无效";
        return false;
    }
    // DMABUF and VULKAN frames share the plane contract: plane_count planes
    // with the first plane's fd materialized as the local handle alias.
    if (frame->memory_type == KOPAW_MEMORY_DMABUF ||
        frame->memory_type == KOPAW_MEMORY_VULKAN) {
        if (frame->plane_count == 0 || frame->plane_count > KOPAW_MAX_DMABUF_PLANES) {
            if (error) *error = "DMA-BUF 平面数无效";
            return false;
        }
        if (frame->dma_buf_handle == 0 && frame->planes[0].fd != 0) {
            if (error) *error = "DMA-BUF 帧缺少 dma_buf_handle";
            return false;
        }
        for (uint32_t i = 0; i < frame->plane_count; ++i) {
            if (frame->planes[i].fd < 0) {
                if (error) *error = "DMA-BUF 平面 fd 无效";
                return false;
            }
        }
        // The first plane is the canonical single-buffer handle. In HYBRID
        // mode it must be the FD materialized in this process, never a source
        // process's numeric FD.
        if (frame->dma_buf_handle !=
            static_cast<uint64_t>(static_cast<uint32_t>(frame->planes[0].fd))) {
            if (error) *error = "dma_buf_handle 与首个 DMA-BUF fd 不匹配";
            return false;
        }
    } else if (frame->memory_type == KOPAW_MEMORY_CPU) {
        if (gpu_mode_is_hybrid(mode)) {
            if (error) *error = "HYBRID 模式不接受 CPU 地址句柄帧";
            return false;
        }
        if (frame->dma_buf_handle == 0 || frame->cpu_size == 0) {
            if (error) *error = "CPU 帧缺少数据";
            return false;
        }
    } else {
        if (error) *error = "未知帧内存类型";
        return false;
    }
    switch (frame->acquire_fence.kind) {
    case KOPAW_SYNC_FENCE_NONE:
        break;
    case KOPAW_SYNC_FENCE_FD:
        if (frame->acquire_fence.fd < 0) {
            if (error) *error = "acquire fence fd 无效";
            return false;
        }
        break;
    case KOPAW_SYNC_FENCE_TIMELINE:
        break;
    default:
        if (error) *error = "未知 acquire fence 类型";
        return false;
    }
    return true;
}

}  // namespace kopms
