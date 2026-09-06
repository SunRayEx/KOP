#include "drm_buffer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>

#ifdef KOPMS_HAVE_LIBDRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#endif

namespace kopms {

namespace {

const DrmKmsPlane* selected_primary_plane(const DrmKmsSnapshot& snapshot) {
    if (!snapshot.selection.valid) return nullptr;
    for (const DrmKmsPlane& plane : snapshot.planes) {
        if (plane.id == snapshot.selection.primary_plane_id) return &plane;
    }
    return nullptr;
}

bool has_modifier(const DrmDmabufImportRequest& request) {
    for (uint32_t i = 0; i < request.plane_count; ++i) {
        if (request.planes[i].modifier != 0) return true;
    }
    return false;
}

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

#ifdef KOPMS_HAVE_LIBDRM
void set_drm_error(std::string* error, const char* prefix, int result) {
    if (!error) return;
    *error = prefix;
    *error += std::strerror(errno);
    *error += " (" + std::to_string(result) + ")";
}

void close_unique_handles(int drm_fd,
                         const std::array<uint32_t, KOPAW_MAX_DMABUF_PLANES>& handles,
                         uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (handles[i] == 0) continue;
        bool duplicate = false;
        for (uint32_t j = 0; j < i; ++j) {
            if (handles[j] == handles[i]) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) drmCloseBufferHandle(drm_fd, handles[i]);
    }
}
#endif

}  // namespace

bool validate_dmabuf_import(const DrmDmabufImportRequest& request, int drm_fd,
                            const DrmKmsSnapshot& snapshot, std::string* error) {
    if (drm_fd < 0) {
        set_error(error, "DRM fd 无效");
        return false;
    }
    if (request.media_type != KOPAW_MEDIA_VIDEO) {
        set_error(error, "KMS 只接受视频 DMA-BUF");
        return false;
    }
    if (request.width == 0 || request.height == 0) {
        set_error(error, "DMA-BUF 尺寸无效");
        return false;
    }
    if (request.format == 0) {
        set_error(error, "DMA-BUF format 无效");
        return false;
    }
    if (request.plane_count == 0 || request.plane_count > KOPAW_MAX_DMABUF_PLANES) {
        set_error(error, "DMA-BUF plane 数无效");
        return false;
    }
    const DrmKmsPlane* plane = selected_primary_plane(snapshot);
    if (!plane) {
        set_error(error, "没有可用的 primary plane");
        return false;
    }
    if (plane->formats.empty() ||
        std::find(plane->formats.begin(), plane->formats.end(), request.format) ==
            plane->formats.end()) {
        set_error(error, "primary plane 不支持该像素格式");
        return false;
    }
    for (uint32_t i = 0; i < request.plane_count; ++i) {
        const DrmDmabufImportPlane& source = request.planes[i];
        if (source.fd < 0 || source.fd == drm_fd) {
            set_error(error, "DMA-BUF source fd 无效");
            return false;
        }
        if (source.stride == 0) {
            set_error(error, "DMA-BUF stride 无效");
            return false;
        }
    }
    if (has_modifier(request) && !snapshot.addfb2_modifiers) {
        set_error(error, "DRM 设备不支持 framebuffer modifiers");
        return false;
    }
    switch (request.acquire_fence_kind) {
        case KOPAW_SYNC_FENCE_NONE:
            if (request.acquire_fence_fd >= 0) {
                set_error(error, "无 fence 类型不应携带 fence fd");
                return false;
            }
            break;
        case KOPAW_SYNC_FENCE_FD:
            if (request.acquire_fence_fd < 0 || request.acquire_fence_fd == drm_fd) {
                set_error(error, "acquire fence fd 无效");
                return false;
            }
            break;
        case KOPAW_SYNC_FENCE_TIMELINE:
            set_error(error, "KMS importer 暂不支持 timeline fence");
            return false;
        default:
            set_error(error, "acquire fence 类型无效");
            return false;
    }
    return true;
}

bool wait_for_acquire_fence(const DrmDmabufImportRequest& request, int timeout_ms,
                            std::string* error) {
    if (request.acquire_fence_kind == KOPAW_SYNC_FENCE_NONE) return true;
    if (request.acquire_fence_kind != KOPAW_SYNC_FENCE_FD ||
        request.acquire_fence_fd < 0) {
        set_error(error, "无法等待 acquire fence");
        return false;
    }
    pollfd descriptor{};
    descriptor.fd = request.acquire_fence_fd;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
        set_error(error, "acquire fence 等待超时");
        return false;
    }
    if (result < 0) {
        if (error) {
            *error = "等待 acquire fence 失败: ";
            *error += std::strerror(errno);
        }
        return false;
    }
    if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0 ||
        (descriptor.revents & (POLLIN | POLLHUP)) == 0) {
        set_error(error, "acquire fence 状态无效");
        return false;
    }
    return true;
}

DrmImportedBuffer::~DrmImportedBuffer() { reset(); }

DrmImportedBuffer::DrmImportedBuffer(DrmImportedBuffer&& other) noexcept
    : drm_fd(other.drm_fd), framebuffer_id(other.framebuffer_id), width(other.width),
      height(other.height), format(other.format), plane_count(other.plane_count),
      gem_handles(other.gem_handles) {
    other.drm_fd = -1;
    other.framebuffer_id = 0;
    other.width = 0;
    other.height = 0;
    other.format = 0;
    other.plane_count = 0;
    other.gem_handles.fill(0);
}

DrmImportedBuffer& DrmImportedBuffer::operator=(DrmImportedBuffer&& other) noexcept {
    if (this == &other) return *this;
    reset();
    drm_fd = other.drm_fd;
    framebuffer_id = other.framebuffer_id;
    width = other.width;
    height = other.height;
    format = other.format;
    plane_count = other.plane_count;
    gem_handles = other.gem_handles;
    other.drm_fd = -1;
    other.framebuffer_id = 0;
    other.width = 0;
    other.height = 0;
    other.format = 0;
    other.plane_count = 0;
    other.gem_handles.fill(0);
    return *this;
}

void DrmImportedBuffer::reset() noexcept {
#ifdef KOPMS_HAVE_LIBDRM
    if (drm_fd >= 0) {
        if (framebuffer_id != 0) drmModeRmFB(drm_fd, framebuffer_id);
        close_unique_handles(drm_fd, gem_handles, plane_count);
    }
#endif
    drm_fd = -1;
    framebuffer_id = 0;
    width = 0;
    height = 0;
    format = 0;
    plane_count = 0;
    gem_handles.fill(0);
}

bool import_dmabuf_frame(int drm_fd, const DrmDmabufImportRequest& request,
                         const DrmKmsSnapshot& snapshot,
                         DrmImportedBuffer* output, int fence_timeout_ms,
                         std::string* error) {
    if (!output) {
        set_error(error, "DMA-BUF import 输出目标为空");
        return false;
    }
    output->reset();
    if (!validate_dmabuf_import(request, drm_fd, snapshot, error)) return false;
    if (!wait_for_acquire_fence(request, fence_timeout_ms, error)) return false;
#ifndef KOPMS_HAVE_LIBDRM
    (void)drm_fd;
    (void)request;
    (void)snapshot;
    set_error(error, "libdrm 不可用，DMA-BUF import 未启用");
    return false;
#else
    std::array<uint32_t, KOPAW_MAX_DMABUF_PLANES> handles{};
    uint32_t imported_count = 0;
    for (uint32_t i = 0; i < request.plane_count; ++i) {
        uint32_t handle = 0;
        for (uint32_t j = 0; j < i; ++j) {
            if (request.planes[j].fd == request.planes[i].fd) {
                handle = handles[j];
                break;
            }
        }
        if (handle == 0 && drmPrimeFDToHandle(drm_fd, request.planes[i].fd, &handle) < 0) {
            set_drm_error(error, "DMA-BUF PRIME_FD_TO_HANDLE 失败", errno);
            close_unique_handles(drm_fd, handles, request.plane_count);
            return false;
        }
        handles[i] = handle;
        if (handle != 0) ++imported_count;
    }

    uint32_t pitches[KOPAW_MAX_DMABUF_PLANES] = {};
    uint32_t offsets[KOPAW_MAX_DMABUF_PLANES] = {};
    uint64_t modifiers[KOPAW_MAX_DMABUF_PLANES] = {};
    for (uint32_t i = 0; i < request.plane_count; ++i) {
        pitches[i] = request.planes[i].stride;
        offsets[i] = request.planes[i].offset;
        modifiers[i] = request.planes[i].modifier;
    }

    uint32_t framebuffer_id = 0;
    const int result = has_modifier(request)
                           ? drmModeAddFB2WithModifiers(
                                 drm_fd, request.width, request.height, request.format,
                                 handles.data(), pitches, offsets, modifiers, &framebuffer_id,
                                 0)
                           : drmModeAddFB2(drm_fd, request.width, request.height, request.format,
                                           handles.data(), pitches, offsets, &framebuffer_id, 0);
    if (result < 0) {
        set_drm_error(error, "创建 DRM framebuffer 失败", result);
        close_unique_handles(drm_fd, handles, imported_count);
        return false;
    }
    output->drm_fd = drm_fd;
    output->framebuffer_id = framebuffer_id;
    output->width = request.width;
    output->height = request.height;
    output->format = request.format;
    output->plane_count = request.plane_count;
    output->gem_handles = handles;
    return true;
#endif
}

}  // namespace kopms
