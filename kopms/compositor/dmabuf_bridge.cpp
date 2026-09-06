#include "dmabuf_bridge.hpp"

#include <unistd.h>

#include <memory>
#include <unordered_map>

#include "kop/log.h"
#include "wayland-server.h"
#include "wayland-server-protocol.h"

#ifdef KOPMS_HAVE_LINUX_DMABUF
#include "linux-dmabuf-server-protocol.h"
#endif
#ifdef KOPMS_HAVE_EXPLICIT_SYNC
#include "linux-explicit-synchronization-server-protocol.h"
#endif

namespace kopms {

static const char* kTag = "kopms-dmabuf";

namespace {

// 本仓库导入面覆盖的单平面 RGBA DRM fourcc（与 vk_scene 的映射一致）。
constexpr uint32_t kSupportedFormats[] = {
    0x34325258u,  // XR24 XRGB8888
    0x34325241u,  // AR24 ARGB8888
    0x34324258u,  // XB24 XBGR8888
    0x34324241u,  // AB24 ABGR8888（KOPAW Vulkan 导出格式）
};
constexpr uint64_t kModifierLinear = 0;
constexpr uint64_t kModifierInvalid = 0x00ffffffffffffffull;

// wl_buffer 用户数据：平面 fd 的最终所有者（随资源销毁关闭）。
struct DmabufBuffer {
    DmabufBufferView view;
};

void dmabuf_buffer_destroyed(wl_resource* resource) {
    auto* buffer = static_cast<DmabufBuffer*>(wl_resource_get_user_data(resource));
    if (!buffer) return;
    for (uint32_t i = 0; i < buffer->view.plane_count; ++i) {
        if (buffer->view.fds[i] >= 0) ::close(buffer->view.fds[i]);
    }
    delete buffer;
}

void dmabuf_buffer_destroy_req(wl_client*, wl_resource* resource) {
    // 客户端显式销毁：析构器（dmabuf_buffer_destroyed）随后关闭平面 fd。
    wl_resource_destroy(resource);
}

const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy_req,
};

// linux_buffer_params 用户数据：add 阶段收集的平面（未创建前归 params 持有）。
struct BufferParams {
    DmabufBufferView view;
    bool used = false;  // create 只允许一次
};

void params_destroyed(wl_resource* resource) {
    auto* params = static_cast<BufferParams*>(wl_resource_get_user_data(resource));
    if (!params) return;
    if (!params->used) {
        // 未物化为 wl_buffer：fd 由 params 负责关闭。
        for (uint32_t i = 0; i < params->view.plane_count; ++i) {
            if (params->view.fds[i] >= 0) ::close(params->view.fds[i]);
        }
    }
    delete params;
}

bool params_add_plane(BufferParams* params, int32_t fd, uint32_t index, int32_t* error_id) {
    if (!params || params->used || index >= KOPAW_MAX_DMABUF_PLANES || fd < 0) {
        if (error_id) *error_id = 1;
        if (fd >= 0) ::close(fd);
        return false;
    }
    // 重复平面 fd 是协议错误（ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX 语义）
    for (uint32_t i = 0; i < KOPAW_MAX_DMABUF_PLANES; ++i) {
        if (params->view.fds[i] == fd && i != index) {
            if (error_id) *error_id = 2;
            ::close(fd);
            return false;
        }
    }
    params->view.fds[index] = fd;
    if (index + 1 > params->view.plane_count) {
        params->view.plane_count = index + 1;
    }
    return true;
}

wl_resource* create_dmabuf_buffer(wl_client* client, wl_resource* params_resource,
                                  uint32_t width, uint32_t height, uint32_t format,
                                  uint32_t flags, uint32_t id, bool immediate,
                                  const struct wl_interface* buffer_interface) {
    auto* params = static_cast<BufferParams*>(wl_resource_get_user_data(params_resource));
    if (!params || params->used || params->view.plane_count == 0) {
        if (!immediate) {
            zwp_linux_buffer_params_v1_send_failed(params_resource);
        } else {
            wl_resource_post_error(params_resource,
                                   ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                                   "params already used");
        }
        return nullptr;
    }
    params->view.width = width;
    params->view.height = height;
    params->view.format = format;
    // flags（Y_INVERT 等）暂不支持：合成器忽略变换是 M1 既有限制。
    (void)flags;

    wl_resource* buffer = wl_resource_create(client, buffer_interface, 1, id);
    if (!buffer) {
        if (!immediate) zwp_linux_buffer_params_v1_send_failed(params_resource);
        return nullptr;
    }
    auto* dmabuf = new DmabufBuffer();
    dmabuf->view = params->view;
    wl_resource_set_implementation(buffer, &dmabuf_buffer_impl, dmabuf,
                                   dmabuf_buffer_destroyed);
    params->used = true;
    if (!immediate) {
        zwp_linux_buffer_params_v1_send_created(params_resource, buffer);
    }
    KOP_LOG_DEBUG(kTag, "dmabuf wl_buffer 创建 %ux%u fmt=0x%x planes=%u",
                  width, height, format, params->view.plane_count);
    return buffer;
}

void params_add(wl_client*, wl_resource* resource, int32_t fd, uint32_t index,
                uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                uint32_t modifier_lo) {
    auto* params = static_cast<BufferParams*>(wl_resource_get_user_data(resource));
    int32_t error_id = 0;
    if (params_add_plane(params, fd, index, &error_id)) {
        params->view.offsets[index] = offset;
        params->view.strides[index] = stride;
        params->view.modifiers[index] =
            (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
    } else {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "invalid plane add");
    }
}

void params_create(wl_client* client, wl_resource* resource, int32_t width,
                   int32_t height, uint32_t format, uint32_t flags) {
    // v1 create：服务端分配 wl_buffer id 并经 created 事件回传。
    create_dmabuf_buffer(client, resource, width, height, format, flags, 0, false,
                         &wl_buffer_interface);
}

void params_create_immed(wl_client* client, wl_resource* resource, uint32_t id,
                         int32_t width, int32_t height, uint32_t format,
                         uint32_t flags) {
    create_dmabuf_buffer(client, resource, width, height, format, flags, id, true,
                         &wl_buffer_interface);
}

void params_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy_req,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

void dmabuf_create_params(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* params = wl_resource_create(
        client, &zwp_linux_buffer_params_v1_interface,
        wl_resource_get_version(resource), id);
    if (!params) return;
    auto* state = new BufferParams();
    wl_resource_set_implementation(params, &params_impl, state, params_destroyed);
}

void dmabuf_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy_req,
    .create_params = dmabuf_create_params,
    .get_default_feedback = nullptr,   // v4 feedback 路径未实现（bind 上限 v3）
    .get_surface_feedback = nullptr,
};

void dmabuf_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    const uint32_t bound = version > 3 ? 3 : version;
    wl_resource* resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                                               bound, id);
    if (!resource) return;
    wl_resource_set_implementation(resource, &dmabuf_impl, nullptr, nullptr);
    // 格式 + modifier 协商（v3 modifier 事件；v1/v2 仅 format）。
    for (uint32_t fourcc : kSupportedFormats) {
        zwp_linux_dmabuf_v1_send_format(resource, fourcc);
        if (bound >= 3) {
            zwp_linux_dmabuf_v1_send_modifier(resource, fourcc,
                                              static_cast<uint32_t>(kModifierLinear >> 32),
                                              static_cast<uint32_t>(kModifierLinear));
            zwp_linux_dmabuf_v1_send_modifier(resource, fourcc,
                                              static_cast<uint32_t>(kModifierInvalid >> 32),
                                              static_cast<uint32_t>(kModifierInvalid));
        }
    }
    KOP_LOG_DEBUG(kTag, "zwp_linux_dmabuf_v1 bind v%u", bound);
}

// ---------------------------------------------------------------------------
// explicit synchronization（unstable v1）
// ---------------------------------------------------------------------------

// surface 键控的同步状态（bridge Impl 持有 map）。
struct SurfaceSync {
    int acquire_fd = -1;         // set_acquire_fence 移交的 fd
    wl_resource* release = nullptr;  // 最近一次 get_release 的对象
};

void sync_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void sync_set_acquire_fence(wl_client*, wl_resource* resource, int32_t fd) {
    auto* sync = static_cast<SurfaceSync*>(wl_resource_get_user_data(resource));
    if (!sync) {
        ::close(fd);
        return;
    }
    if (sync->acquire_fd >= 0) ::close(sync->acquire_fd);
    sync->acquire_fd = fd;
}

void sync_get_release(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* sync = static_cast<SurfaceSync*>(wl_resource_get_user_data(resource));
    if (!sync) return;
    if (sync->release) {
        // 上一 release 对象被覆盖：其 buffer 已不再被引用，immediate。
        zwp_linux_buffer_release_v1_send_immediate_release(sync->release);
        wl_resource_destroy(sync->release);
    }
    wl_resource* release =
        wl_resource_create(client, &zwp_linux_buffer_release_v1_interface, 1, id);
    if (!release) return;
    wl_resource_set_implementation(release, nullptr, nullptr, nullptr);
    sync->release = release;
}

const struct zwp_linux_surface_synchronization_v1_interface sync_impl = {
    .destroy = sync_destroy_req,
    .set_acquire_fence = sync_set_acquire_fence,
    .get_release = sync_get_release,
};

void expsync_get_synchronization(wl_client* client, wl_resource* manager,
                                        uint32_t id, wl_resource* surface) {
    auto* bridge = static_cast<DmabufBridge*>(wl_resource_get_user_data(manager));
    if (!bridge || !surface) return;
    // 同一 surface 复用一份同步状态（多个 sync object 共享）。
    auto* state = static_cast<SurfaceSync*>(bridge->sync_state_for(surface));
    if (!state) return;
    wl_resource* sync = wl_resource_create(
        client, &zwp_linux_surface_synchronization_v1_interface, 1, id);
    if (!sync) return;
    wl_resource_set_implementation(sync, &sync_impl, state, nullptr);
}

void expsync_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

const struct zwp_linux_explicit_synchronization_v1_interface expsync_impl = {
    .destroy = expsync_destroy_req,
    .get_synchronization = expsync_get_synchronization,
};

void expsync_bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(
        client, &zwp_linux_explicit_synchronization_v1_interface,
        version > 1 ? 1 : version, id);
    if (!resource) return;
    wl_resource_set_implementation(resource, &expsync_impl, data, nullptr);
    KOP_LOG_DEBUG(kTag, "zwp_linux_explicit_synchronization_v1 bind");
}

}  // namespace

struct DmabufBridge::Impl {
    wl_display* display = nullptr;
    // surface 资源 → 同步状态（explicit sync 的 acquire/release 记账）。
    // SurfaceSync 由 sync object 共享，map 持有所有权。
    std::unordered_map<wl_resource*, std::unique_ptr<SurfaceSync>> sync_by_surface;
};

DmabufBridge::~DmabufBridge() { delete impl_; }

bool DmabufBridge::install(wl_display* display, std::string* error) {
    if (!display) {
        if (error) *error = "dmabuf bridge 需要 wl_display";
        return false;
    }
    delete impl_;
    impl_ = new Impl();
    impl_->display = display;
#ifdef KOPMS_HAVE_LINUX_DMABUF
    wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 3, this, dmabuf_bind);
    dmabuf_enabled_ = true;
#else
    (void)error;
    KOP_LOG_INFO(kTag, "linux-dmabuf 协议未编译（系统缺 wayland-protocols XML）");
#endif
#ifdef KOPMS_HAVE_EXPLICIT_SYNC
    wl_global_create(display, &zwp_linux_explicit_synchronization_v1_interface, 1,
                     this, expsync_bind);
    explicit_sync_enabled_ = true;
#endif
    return true;
}

bool DmabufBridge::is_dmabuf_buffer(wl_resource* buffer) {
    return buffer && wl_resource_instance_of(buffer, &wl_buffer_interface,
                                             &dmabuf_buffer_impl);
}

bool DmabufBridge::buffer_view(wl_resource* buffer, DmabufBufferView* view,
                               int* acquire_fence_fd) {
    if (acquire_fence_fd) *acquire_fence_fd = -1;
    if (!is_dmabuf_buffer(buffer) || !view) return false;
    auto* dmabuf = static_cast<DmabufBuffer*>(wl_resource_get_user_data(buffer));
    if (!dmabuf) return false;
    *view = dmabuf->view;
    return true;
}

void DmabufBridge::send_buffer_release(wl_resource* buffer) {
    if (is_dmabuf_buffer(buffer)) wl_buffer_send_release(buffer);
}

int DmabufBridge::take_acquire_fence(wl_resource* surface_resource) {
    if (!impl_) return -1;
    auto it = impl_->sync_by_surface.find(surface_resource);
    if (it == impl_->sync_by_surface.end()) return -1;
    int fd = it->second->acquire_fd;
    it->second->acquire_fd = -1;
    return fd;
}

void* DmabufBridge::sync_state_for(wl_resource* surface_resource) {
    if (!impl_) return nullptr;
    auto& slot = impl_->sync_by_surface[surface_resource];
    if (!slot) slot = std::make_unique<SurfaceSync>();
    return slot.get();
}

void DmabufBridge::fire_release(wl_resource* surface_resource, int release_fence_fd) {
    if (!impl_) {
        if (release_fence_fd >= 0) ::close(release_fence_fd);
        return;
    }
    auto it = impl_->sync_by_surface.find(surface_resource);
    if (it == impl_->sync_by_surface.end() || !it->second->release) {
        if (release_fence_fd >= 0) ::close(release_fence_fd);
        return;
    }
    wl_resource* release = it->second->release;
    it->second->release = nullptr;
    if (release_fence_fd >= 0) {
        zwp_linux_buffer_release_v1_send_fenced_release(release, release_fence_fd);
    } else {
        zwp_linux_buffer_release_v1_send_immediate_release(release);
    }
    wl_resource_destroy(release);
}

void DmabufBridge::surface_destroyed(wl_resource* surface_resource) {
    if (!impl_) return;
    impl_->sync_by_surface.erase(surface_resource);
}

}  // namespace kopms
