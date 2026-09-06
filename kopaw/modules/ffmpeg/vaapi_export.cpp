#include "vaapi_export.hpp"

#include <unistd.h>

#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixfmt.h>
#include <va/va_drmcommon.h>
}

#include "kop/log.h"

namespace kopaw {

static const char* kTag = "va-export";

bool VaapiDmabufExporter::init(VADisplay display) {
    if (!display) return false;
    // display 由解码器的 AVVAAPIDeviceContext 初始化并持有；这里只借用，
    // 不重复 vaInitialize（引用计数会失衡）。生命周期覆盖全部导出帧：
    // 帧引用全部在 stop 时归还，早于节点的 device_ref 释放。
    display_ = display;
    KOP_LOG_INFO(kTag, "VAAPI DMA-BUF 导出器就绪");
    return true;
}

OwnedFrame* VaapiDmabufExporter::export_frame(const AVFrame* hw, std::string* error) {
    if (!display_) {
        if (error) *error = "导出器未初始化";
        return nullptr;
    }
    if (!hw || hw->format != AV_PIX_FMT_VAAPI || !hw->hw_frames_ctx ||
        !hw->hw_frames_ctx->data) {
        if (error) *error = "非 VAAPI 硬解帧";
        return nullptr;
    }
    const auto* fctx =
        reinterpret_cast<const AVHWFramesContext*>(hw->hw_frames_ctx->data);
    if (fctx->sw_format != AV_PIX_FMT_NV12 && fctx->sw_format != AV_PIX_FMT_P010LE) {
        // P2 覆盖 8bit NV12 与 10bit P010（HEVC Main10）；其余格式回退软拷贝
        if (error) *error = "表面格式非 NV12/P010，暂不支持原生导出";
        return nullptr;
    }
    const auto surface = static_cast<VASurfaceID>(
        reinterpret_cast<uintptr_t>(hw->data[3]));
    if (surface == VA_INVALID_ID) {
        if (error) *error = "无效的 VASurfaceID";
        return nullptr;
    }

    // 解码管线无跨进程 fence 可导出：同步等待解码完成，保证导入方立即可读
    VAStatus st = vaSyncSurface(display_, surface);
    if (st != VA_STATUS_SUCCESS) {
        if (!sync_warned_) {
            sync_warned_ = true;
            KOP_LOG_WARN(kTag, "vaSyncSurface 失败（0x%x），继续尝试导出", st);
        }
    }

    VADRMPRIMESurfaceDescriptor prime{};
    st = vaExportSurfaceHandle(display_, surface,
                               VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_ONLY |
                                   VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                               &prime);
    if (st != VA_STATUS_SUCCESS) {
        if (error)
            *error = "vaExportSurfaceHandle 失败（0x" +
                     std::to_string(static_cast<int>(st)) + "）";
        return nullptr;
    }
    // 本实现只接受单对象双平面的组合层导出（NV12/P010，i915/iHD 标准形态）；
    // 对象数 >1 的驱动形态导入方无法用单份外部内存描述，回退软拷贝路径
    if (prime.num_objects != 1 || prime.num_layers != 1 ||
        prime.layers[0].num_planes != 2) {
        for (uint32_t i = 0; i < prime.num_objects; ++i) close(prime.objects[i].fd);
        if (error) *error = "不支持的导出布局（objects/layers/planes 不符）";
        return nullptr;
    }

    OwnedFrame* o = make_external_frame(KOPAW_MEDIA_VIDEO, hw->pts, hw->pts);
    o->frame.memory_type = KOPAW_MEMORY_DMABUF;
    o->frame.drm_fourcc = prime.layers[0].drm_format;
    o->frame.format.video.width = static_cast<uint32_t>(hw->width);
    o->frame.format.video.height = static_cast<uint32_t>(hw->height);
    o->frame.size = static_cast<uintptr_t>(prime.objects[0].size);
    o->frame.dma_fd = prime.objects[0].fd;
    // DMABUF 帧的本地句柄约定为首个对象 fd（跨进程传输走协议层的 fd 通道）
    o->frame.dma_buf_handle =
        static_cast<uint64_t>(static_cast<uintptr_t>(prime.objects[0].fd));
    o->frame.plane_count = 2;
    // VALayerDescriptor 的平面元数据为数组形态：offset[k]/stride[k]，对象经
    // object_index[k] 指向（本实现已校验单对象布局，恒为 objects[0]）
    for (uint32_t i = 0; i < 2; ++i) {
        o->frame.planes[i].fd = prime.objects[0].fd;
        o->frame.planes[i].offset = prime.layers[0].offset[i];
        o->frame.planes[i].stride = prime.layers[0].pitch[i];
        o->frame.planes[i].modifier = prime.objects[0].drm_format_modifier;
    }
    // acquire_fence：导出前已 vaSyncSurface，数据就绪，置 NONE
    o->frame.acquire_fence.kind = KOPAW_SYNC_FENCE_NONE;

    // 持有表面引用：release 归零后表面回到解码器池，fd 先行关闭
    AVFrame* keep = av_frame_clone(const_cast<AVFrame*>(hw));
    if (!keep) {
        // 无法持引用：表面可能被复用，必须放弃本次导出
        close(prime.objects[0].fd);
        if (error) *error = "AVFrame 克隆失败，放弃导出";
        delete o;
        return nullptr;
    }
    o->external_ctx = keep;
    o->external_release = [](void* ctx, OwnedFrame*) {
        if (ctx) av_frame_free(reinterpret_cast<AVFrame**>(&ctx));
    };
    KOP_LOG_DEBUG(kTag, "导出 %dx%d fourcc=0x%08x fd=%d modifier=0x%llx stride=%d/%d",
                  hw->width, hw->height, prime.layers[0].drm_format,
                  prime.objects[0].fd,
                  static_cast<unsigned long long>(prime.objects[0].drm_format_modifier),
                  prime.layers[0].pitch[0], prime.layers[0].pitch[1]);
    return o;
}

}  // namespace kopaw
