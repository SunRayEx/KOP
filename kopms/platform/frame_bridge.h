// KOPMS P3-M3 帧描述与生命周期边界。
//
// 该接口与当前 wl_shm commit 路径刻意分离。KOPAW 生产者可以把 GPU
// buffer 的 DMA-BUF 平面、modifier 和 acquire fence 交给合成器；合成器
// 只在完成引用后调用 release，绝不能自行关闭这些 fd。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kopaw_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOPMS_FRAME_DESCRIPTOR_VERSION 2

typedef struct KopmsFrameDescriptor KopmsFrameDescriptor;

typedef void (*KopmsFrameRetainFn)(KopmsFrameDescriptor* frame);
typedef void (*KopmsFrameReleaseFn)(KopmsFrameDescriptor* frame);

typedef struct KopmsFrameDescriptor {
    uint32_t struct_size;
    uint32_t version;
    KopawMediaType media_type;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t memory_type;
    uint32_t plane_count;
    KopawDmabufPlane planes[KOPAW_MAX_DMABUF_PLANES];
    KopawSyncFence acquire_fence;
    // CPU/SINGLE: process-local address alias. HYBRID/DMABUF: local DMA-BUF
    // handle, normally the FD received through the transport layer.
    uint64_t dma_buf_handle;
    size_t cpu_size;
    void* user_data;
    KopmsFrameRetainFn retain;
    KopmsFrameReleaseFn release;
    // Optional tail fields. Producers with the shorter v2 prefix may leave
    // these absent; consumers must check struct_size before reading them.
    int64_t pts;
    int64_t dts;
    // Optional colorimetry/HDR tail. Consumers must gate reads by struct_size.
    KopawColorMetadata color;
} KopmsFrameDescriptor;

#ifdef __cplusplus
}

#include <string>

#include "gpu_mode.hpp"

namespace kopms {

bool validate_frame_descriptor(const KopmsFrameDescriptor* frame, std::string* error);
bool validate_frame_descriptor(const KopmsFrameDescriptor* frame, GpuMode mode,
                               std::string* error);

// A lease is a scoped reference to a descriptor. It is the intended handoff
// object for the future Vulkan/DRM compositor path.
class FrameLease {
public:
    explicit FrameLease(KopmsFrameDescriptor* frame) : frame_(frame) {}
    FrameLease(const FrameLease& other) : frame_(other.frame_) {
        if (frame_ && frame_->retain) frame_->retain(frame_);
    }
    FrameLease& operator=(const FrameLease& other) {
        if (this == &other) return *this;
        reset(other.frame_);
        if (frame_ && frame_->retain) frame_->retain(frame_);
        return *this;
    }
    FrameLease(FrameLease&& other) noexcept : frame_(other.frame_) { other.frame_ = nullptr; }
    FrameLease& operator=(FrameLease&& other) noexcept {
        if (this == &other) return *this;
        reset();
        frame_ = other.frame_;
        other.frame_ = nullptr;
        return *this;
    }
    ~FrameLease() { reset(); }

    KopmsFrameDescriptor* get() const { return frame_; }
    void reset(KopmsFrameDescriptor* frame = nullptr) {
        if (frame_ && frame_->release) frame_->release(frame_);
        frame_ = frame;
    }

private:
    KopmsFrameDescriptor* frame_ = nullptr;
};

}  // namespace kopms
#endif
