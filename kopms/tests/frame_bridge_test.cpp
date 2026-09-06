#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>

#include "frame_bridge.h"

namespace {

struct Lifetime {
    std::atomic<int> retained{0};
    std::atomic<int> released{0};
};

void retain(KopmsFrameDescriptor* frame) {
    static_cast<Lifetime*>(frame->user_data)->retained.fetch_add(1);
}

void release(KopmsFrameDescriptor* frame) {
    static_cast<Lifetime*>(frame->user_data)->released.fetch_add(1);
}

bool expect_invalid(const KopmsFrameDescriptor* frame, kopms::GpuMode mode,
                    const char* name) {
    std::string error;
    if (kopms::validate_frame_descriptor(frame, mode, &error)) {
        std::fprintf(stderr, "%s unexpectedly validated\n", name);
        return false;
    }
    return true;
}

bool run() {
    if (!expect_invalid(nullptr, kopms::GpuMode::Single, "null descriptor")) return false;

    Lifetime lifetime;
    uint8_t pixels[16] = {};
    KopmsFrameDescriptor frame{};
    frame.struct_size = sizeof(frame);
    frame.version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    frame.media_type = KOPAW_MEDIA_VIDEO;
    frame.width = 2;
    frame.height = 2;
    frame.format = 0x34325241;  // DRM_FORMAT_AR24, diagnostic-only here.
    frame.memory_type = KOPAW_MEMORY_CPU;
    frame.dma_buf_handle =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pixels));
    frame.cpu_size = sizeof(pixels);
    frame.user_data = &lifetime;
    frame.retain = retain;
    frame.release = release;

    std::string error;
    if (!kopms::validate_frame_descriptor(&frame, kopms::GpuMode::Single, &error)) {
        std::fprintf(stderr, "valid CPU descriptor rejected: %s\n", error.c_str());
        return false;
    }
    if (!expect_invalid(&frame, kopms::GpuMode::Hybrid,
                        "CPU descriptor in HYBRID mode")) {
        return false;
    }

    {
        kopms::FrameLease first(&frame);
        kopms::FrameLease second = first;
        if (lifetime.retained.load() != 1) return false;
        second.reset();
        if (lifetime.released.load() != 1) return false;
    }
    if (lifetime.released.load() != 2) return false;

    KopmsFrameDescriptor short_frame = frame;
    short_frame.struct_size = static_cast<uint32_t>(offsetof(KopmsFrameDescriptor, release));
    if (!expect_invalid(&short_frame, kopms::GpuMode::Single, "short descriptor")) return false;

    KopmsFrameDescriptor old_version = frame;
    old_version.version = 1;
    if (!expect_invalid(&old_version, kopms::GpuMode::Single,
                        "old descriptor version")) {
        return false;
    }

    KopmsFrameDescriptor bad_media = frame;
    bad_media.media_type = 99;
    if (!expect_invalid(&bad_media, kopms::GpuMode::Single, "bad media type")) return false;

    KopmsFrameDescriptor bad_fence = frame;
    bad_fence.acquire_fence.kind = 99;
    if (!expect_invalid(&bad_fence, kopms::GpuMode::Single, "bad fence")) return false;

    KopmsFrameDescriptor dmabuf = frame;
    dmabuf.memory_type = KOPAW_MEMORY_DMABUF;
    dmabuf.dma_buf_handle = 42;
    dmabuf.cpu_size = 0;
    dmabuf.plane_count = 1;
    dmabuf.planes[0].fd = 42;
    if (!kopms::validate_frame_descriptor(&dmabuf, kopms::GpuMode::Hybrid, &error)) {
        std::fprintf(stderr, "valid DMA-BUF descriptor rejected: %s\n", error.c_str());
        return false;
    }

    KopmsFrameDescriptor bad_handle = dmabuf;
    bad_handle.dma_buf_handle = 43;
    if (!expect_invalid(&bad_handle, kopms::GpuMode::Hybrid,
                        "mismatched DMA-BUF handle")) {
        return false;
    }

    dmabuf.planes[0].fd = -1;
    return expect_invalid(&dmabuf, kopms::GpuMode::Hybrid, "bad DMA-BUF fd");
}

}  // namespace

int main() { return run() ? 0 : 1; }
