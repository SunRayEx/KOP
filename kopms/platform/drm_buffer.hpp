// P3-M2.2 DMA-BUF import boundary.
//
// Import requests borrow their source and fence FDs. The importer never closes
// those FDs; the caller keeps them alive until the resulting frame lease is
// released. GEM handles and the optional framebuffer are owned by the result.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "drm_kms.hpp"
#include "kopaw_abi.h"

namespace kopms {

struct DrmDmabufImportPlane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0;
};

struct DrmDmabufImportRequest {
    uint32_t media_type = KOPAW_MEDIA_VIDEO;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t plane_count = 0;
    std::array<DrmDmabufImportPlane, KOPAW_MAX_DMABUF_PLANES> planes{};
    uint32_t acquire_fence_kind = KOPAW_SYNC_FENCE_NONE;
    int acquire_fence_fd = -1;
};

bool validate_dmabuf_import(const DrmDmabufImportRequest& request,
                            int drm_fd, const DrmKmsSnapshot& snapshot,
                            std::string* error);

// Wait for an explicit sync_file fence without taking ownership of its FD.
// timeout_ms < 0 means wait indefinitely.
bool wait_for_acquire_fence(const DrmDmabufImportRequest& request, int timeout_ms,
                            std::string* error);

struct DrmImportedBuffer {
    DrmImportedBuffer() = default;
    ~DrmImportedBuffer();

    DrmImportedBuffer(const DrmImportedBuffer&) = delete;
    DrmImportedBuffer& operator=(const DrmImportedBuffer&) = delete;
    DrmImportedBuffer(DrmImportedBuffer&& other) noexcept;
    DrmImportedBuffer& operator=(DrmImportedBuffer&& other) noexcept;

    void reset() noexcept;
    bool valid() const noexcept { return framebuffer_id != 0; }

    int drm_fd = -1;  // Borrowed: must outlive this object.
    uint32_t framebuffer_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t plane_count = 0;
    std::array<uint32_t, KOPAW_MAX_DMABUF_PLANES> gem_handles{};
};

bool import_dmabuf_frame(int drm_fd, const DrmDmabufImportRequest& request,
                         const DrmKmsSnapshot& snapshot,
                         DrmImportedBuffer* output, int fence_timeout_ms,
                         std::string* error);

}  // namespace kopms
