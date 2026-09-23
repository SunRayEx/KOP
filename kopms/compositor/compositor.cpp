// KOPMS P3-M1/M3/M4 合成器：Wayland 核心协议、xdg-shell、wl_shm 与
// Vulkan GPU 场景（DMA-BUF 导入/多窗口合成/回压）。
//
// 两条内容路径刻意分离：
//   - M1 CPU wl_shm 路径：嵌套输出同步复制，仅用于 Wayland 协议验证；
//   - M3/M4 GPU 路径：KOPAW BUS 帧（KopmsFrameDescriptor）与 Wayland
//     linux-dmabuf wl_buffer 都经 VulkanScene 导入合成，release 在
//     present/fence/page-flip 完成后才发生，绝不复用 wl_shm 路径。
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <deque>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "wayland-server.h"
#include "wayland-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include "drm_kms.hpp"
#include "drm_hotplug.hpp"
#include "drm_output.hpp"
#include "gpu_mode.hpp"
#include "kopms_server.h"
#include "libinput_guard.hpp"
#include "seat_session.hpp"
#include "kop/log.h"
#include "dmabuf_bridge.hpp"
#include "input_state.hpp"
#include "nested_output.hpp"
#include "vk_scene.hpp"

static const char* kTag = "kopms";

using kopms::NestedOutput;

namespace {

// 隐式场景窗口基址：未做 WINDOW_ATTACH 的 BUS 会话落在该命名空间。
constexpr uint64_t kImplicitSceneWindowBase = 0x9000000000000000ull;

struct RuntimeOptions {
    std::string socket_name = "kop-0";
    std::string bus_socket_name;
    int seconds = 15;
    bool probe_drm = false;
    std::string drm_device = "/dev/dri/card0";
    bool enable_input = false;
    std::string seat = "seat0";
    bool direct_drm = false;
    bool allow_unmanaged_drm = false;
    bool watch_drm = false;
    bool scene_window = false;  // Vulkan 场景用独立 GLFW 窗口呈现（默认离屏）
    kopms::GpuMode gpu_mode = kopms::GpuMode::Single;
    std::string gpu_mode_diagnostic;
};

bool env_flag(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0;
}

bool parse_seconds(const char* text, int* seconds) {
    if (!text || !seconds || text[0] == '\0') return false;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > INT_MAX) return false;
    *seconds = static_cast<int>(value);
    return true;
}

void print_usage(const char* program) {
    std::fprintf(stderr,
                 "用法: %s [socket] [seconds] [选项]\n"
                 "  --probe-drm             枚举 DRM 资源，不取得 master 或执行 modeset\n"
                 "  --drm-device PATH       DRM 设备路径（同时启用探测）\n"
                 "  --enable-input          绑定 libinput seat 并排空事件（不注入输入）\n"
                 "  --seat NAME             libinput seat，默认 seat0\n"
                 "  --direct-drm            显式启用 DRM 物理直出，失败时回退 nested\n"
                 "  --allow-unmanaged-drm   允许没有 logind 授权的 DRM 直出（高风险）\n"
                 "  --watch-drm             监听 DRM udev 热插拔并尝试恢复\n"
                 "  --bus-socket NAME       KOPMS-S BUS socket（默认 <wayland>.bus）\n"
                 "  --scene-window          Vulkan 场景经独立窗口呈现（默认离屏）\n"
                 "  KOPMS_GPU_MODE=SINGLE|HYBRID  媒体句柄模式（默认 SINGLE）\n",
                 program ? program : "kopms-compositor");
}

bool parse_options(int argc, char** argv, RuntimeOptions* options) {
    if (!options) return false;
    if (const char* value = std::getenv("KOPMS_DRM_DEVICE"); value && value[0] != '\0') {
        options->drm_device = value;
    }
    if (const char* value = std::getenv("KOPMS_SEAT"); value && value[0] != '\0') {
        options->seat = value;
    }
    if (const char* value = std::getenv("KOPMS_BUS_SOCKET"); value && value[0] != '\0') {
        options->bus_socket_name = value;
    }
    options->probe_drm = env_flag("KOPMS_PROBE_DRM");
    options->enable_input = env_flag("KOPMS_ENABLE_INPUT");
    options->direct_drm = env_flag("KOPMS_DIRECT_DRM");
    options->allow_unmanaged_drm = env_flag("KOPMS_ALLOW_UNMANAGED_DRM");
    options->watch_drm = options->direct_drm || env_flag("KOPMS_DRM_HOTPLUG");
    options->gpu_mode = kopms::gpu_mode_from_environment(&options->gpu_mode_diagnostic);

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--probe-drm") == 0) {
            options->probe_drm = true;
        } else if (std::strcmp(arg, "--enable-input") == 0) {
            options->enable_input = true;
        } else if (std::strcmp(arg, "--direct-drm") == 0) {
            options->direct_drm = true;
        } else if (std::strcmp(arg, "--allow-unmanaged-drm") == 0) {
            options->allow_unmanaged_drm = true;
        } else if (std::strcmp(arg, "--watch-drm") == 0) {
            options->watch_drm = true;
        } else if (std::strcmp(arg, "--drm-device") == 0 ||
                   std::strcmp(arg, "--seat") == 0 ||
                   std::strcmp(arg, "--bus-socket") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return false;
            if (std::strcmp(arg, "--drm-device") == 0) {
                options->drm_device = argv[++i];
                options->probe_drm = true;
            } else if (std::strcmp(arg, "--seat") == 0) {
                options->seat = argv[++i];
            } else {
                options->bus_socket_name = argv[++i];
            }
        } else if (std::strcmp(arg, "--scene-window") == 0) {
            options->scene_window = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            return false;
        } else if (arg[0] == '-') {
            return false;
        } else if (positional++ == 0) {
            options->socket_name = arg;
        } else if (positional == 2) {
            if (!parse_seconds(arg, &options->seconds)) return false;
        } else {
            return false;
        }
    }
    if (options->bus_socket_name.empty()) {
        options->bus_socket_name = options->socket_name + ".bus";
    }
    if (options->direct_drm) options->watch_drm = true;
    return true;
}

struct Surface;

struct FrameCallback {
    Surface* surface = nullptr;
    wl_resource* resource = nullptr;
};

struct Surface {
    wl_resource* resource = nullptr;
    wl_resource* xdg_surface = nullptr;
    wl_resource* xdg_toplevel = nullptr;
    wl_resource* pending_buffer = nullptr;
    wl_listener pending_buffer_destroy{};
    bool pending_buffer_listener = false;
    std::list<std::unique_ptr<FrameCallback>> frame_cbs;
    std::string title;
    uint32_t configure_serial = 0;
    bool configured = false;
    bool destroyed = false;
    // M4：surface 在 Vulkan 场景中的窗口 id（首个 dmabuf commit 时分配）
    uint64_t scene_window_id = 0;
    // 输入聚焦：最近一次带 buffer 的 commit 序号（主 surface 优先）
    uint64_t commit_seq = 0;
};

struct ClientInfo {
    wl_client* client = nullptr;
    wl_listener destroy{};
};

wl_display* g_display = nullptr;
NestedOutput* g_output = nullptr;
std::list<std::unique_ptr<Surface>> g_surfaces;
std::list<std::unique_ptr<ClientInfo>> g_clients;

// ---------------------------------------------------------------------------
// P3-M3/M4：Vulkan 场景与场景窗口注册表
// ---------------------------------------------------------------------------
kopms::VulkanScene g_scene;
kopms::DmabufBridge g_dmabuf;
kopms::KopmsServer* g_bus_server = nullptr;
kopms::InputState g_input;

struct SceneWindow {
    enum class Kind { Bus, Wayland } kind = Kind::Bus;
    uint32_t content_w = 0;
    uint32_t content_h = 0;
    // Wayland
    wl_resource* buffer = nullptr;
    wl_resource* surface = nullptr;
    // BUS
    uint64_t session = 0;
    uint32_t frame_id = 0;
    uint64_t control_id = 0;  // 关联 CONTROL 窗口（0 = 隐式）
};
std::unordered_map<uint64_t, SceneWindow> g_scene_windows;
uint64_t g_next_wayland_scene_window = 1;

// 每窗口的释放上下文 FIFO：handler 提交时入队，场景 GPU 完成（fire_releases）
// 时按序出队并执行释放。场景对同一窗口的完成顺序 = 提交顺序（单 pending +
// 有序 presented），FIFO 保证释放身份与帧一一对应。
struct SceneReleaseContext {
    SceneWindow::Kind kind = SceneWindow::Kind::Bus;
    // Wayland
    wl_resource* buffer = nullptr;
    wl_resource* surface = nullptr;
    // BUS
    uint64_t session = 0;
    uint32_t frame_id = 0;
};
std::unordered_map<uint64_t, std::deque<SceneReleaseContext>> g_scene_release_queue;

Surface* surface_from(wl_resource* resource) {
    return resource ? static_cast<Surface*>(wl_resource_get_user_data(resource)) : nullptr;
}

void frame_callback_destroy(wl_resource* resource) {
    auto* callback = resource
                        ? static_cast<FrameCallback*>(wl_resource_get_user_data(resource))
                        : nullptr;
    if (!callback) return;
    Surface* surface = callback->surface;
    if (surface) {
        surface->frame_cbs.remove_if([callback](const std::unique_ptr<FrameCallback>& item) {
            return item.get() == callback;
        });
    } else {
        delete callback;
    }
}

void cancel_frame_callbacks(Surface* surface) {
    if (!surface) return;
    while (!surface->frame_cbs.empty()) {
        wl_resource_destroy(surface->frame_cbs.front()->resource);
    }
}

void send_frame_callbacks(Surface* surface, uint32_t time_ms) {
    if (!surface) return;
    while (!surface->frame_cbs.empty()) {
        wl_resource* callback = surface->frame_cbs.front()->resource;
        wl_callback_send_done(callback, time_ms);
        wl_resource_destroy(callback);
    }
}

void pending_buffer_destroyed(wl_listener* listener, void*) {
    Surface* surface = wl_container_of(listener, surface, pending_buffer_destroy);
    surface->pending_buffer = nullptr;
    surface->pending_buffer_listener = false;
    wl_list_init(&surface->pending_buffer_destroy.link);
}

void release_pending_buffer(Surface* surface) {
    if (!surface || !surface->pending_buffer) return;
    wl_resource* buffer = surface->pending_buffer;
    surface->pending_buffer = nullptr;
    if (surface->pending_buffer_listener) {
        wl_list_remove(&surface->pending_buffer_destroy.link);
        wl_list_init(&surface->pending_buffer_destroy.link);
        surface->pending_buffer_listener = false;
    }
    // M1's output is synchronous, so replacing an uncommitted buffer also
    // means the compositor has stopped using it.
    wl_buffer_send_release(buffer);
}

void track_pending_buffer(Surface* surface, wl_resource* buffer) {
    surface->pending_buffer = buffer;
    surface->pending_buffer_destroy.notify = pending_buffer_destroyed;
    wl_list_init(&surface->pending_buffer_destroy.link);
    wl_resource_add_destroy_listener(buffer, &surface->pending_buffer_destroy);
    surface->pending_buffer_listener = true;
}

uint32_t now_millis() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() %
        1000000);
}

// 场景 GPU 完成（fence / flip）后的帧释放入口。
// 释放身份从每窗口 FIFO 队列出队（与提交一一对应），绝不读"最新条目"——
// 否则多帧在飞时释放错位一格，首帧永远得不到 release（实测缺陷）。
void on_scene_release(uint64_t window_id, uint64_t session_id, uint32_t frame_id) {
    if (getenv("KOPMS_RELEASE_DEBUG")) {
        KOP_LOG_DEBUG(kTag, "on_scene_release win=%llu session=%llu frame=%u",
                      static_cast<unsigned long long>(window_id),
                      static_cast<unsigned long long>(session_id), frame_id);
    }
    auto qit = g_scene_release_queue.find(window_id);
    if (qit == g_scene_release_queue.end() || qit->second.empty()) {
        // 无上下文（异常序）：BUS 帧按回调参数兜底释放，防生产端挂死。
        if (g_bus_server && session_id != 0) {
            g_bus_server->release_frame(session_id, frame_id);
        }
        return;
    }
    SceneReleaseContext ctx = qit->second.front();
    qit->second.pop_front();
    if (qit->second.empty()) {
        g_scene_release_queue.erase(qit);
        g_scene_windows.erase(window_id);  // 布局同步：窗口内容已清空
    }
    if (ctx.kind == SceneWindow::Kind::Bus) {
        if (g_bus_server) {
            g_bus_server->release_frame(ctx.session, ctx.frame_id);
        }
        return;
    }
    if (ctx.buffer) kopms::DmabufBridge::send_buffer_release(ctx.buffer);
    if (ctx.surface) {
        g_dmabuf.fire_release(ctx.surface, g_scene.take_release_fence());
        if (Surface* surface = surface_from(ctx.surface)) {
            send_frame_callbacks(surface, now_millis());
        }
    }
}

// Wayland dmabuf wl_buffer → KOPMS Handle → Vulkan 场景（M4.1-4.3）。
void present_dmabuf_buffer(Surface* surface, wl_resource* buffer) {
    if (!surface || !buffer) return;
    // buffer 所有权交给场景注册表：解除 pending 追踪，避免 surface 销毁
    // 时误 release 场景仍持有的内容。
    if (surface->pending_buffer_listener) {
        wl_list_remove(&surface->pending_buffer_destroy.link);
        wl_list_init(&surface->pending_buffer_destroy.link);
        surface->pending_buffer_listener = false;
    }
    surface->pending_buffer = nullptr;

    int acquire_fd = g_dmabuf.take_acquire_fence(surface->resource);
    kopms::DmabufBufferView view;
    if (!g_scene.inited() ||
        !kopms::DmabufBridge::buffer_view(buffer, &view, nullptr) ||
        view.plane_count == 0) {
        if (acquire_fd >= 0) ::close(acquire_fd);
        kopms::DmabufBridge::send_buffer_release(buffer);
        g_dmabuf.fire_release(surface->resource, -1);
        send_frame_callbacks(surface, now_millis());
        KOP_LOG_WARN(kTag, "dmabuf buffer 被拒收（场景未就绪或视图无效）");
        return;
    }

    if (surface->scene_window_id == 0) {
        surface->scene_window_id = g_next_wayland_scene_window++;
    }
    kopms::VulkanScene::ImportRequest request{};
    request.width = view.width;
    request.height = view.height;
    request.format = view.format;
    request.plane_count = view.plane_count;
    request.plane_fds = view.fds;
    request.plane_offsets = view.offsets;
    request.plane_strides = view.strides;
    request.plane_modifiers = view.modifiers;
    request.acquire_fence_kind = acquire_fd >= 0 ? KOPAW_SYNC_FENCE_FD
                                                 : KOPAW_SYNC_FENCE_NONE;
    request.acquire_fence_fd = acquire_fd;
    request.session_id = 0;
    request.frame_id = 0;

    std::string error;
    if (!g_scene.submit(surface->scene_window_id, request, &error)) {
        if (acquire_fd >= 0) ::close(acquire_fd);
        kopms::DmabufBridge::send_buffer_release(buffer);
        g_dmabuf.fire_release(surface->resource, -1);
        send_frame_callbacks(surface, now_millis());
        KOP_LOG_DEBUG(kTag, "dmabuf 帧拒收（%s）", error.c_str());
        return;
    }
    SceneWindow& win = g_scene_windows[surface->scene_window_id];
    win.kind = SceneWindow::Kind::Wayland;
    win.content_w = view.width;
    win.content_h = view.height;
    win.buffer = buffer;
    win.surface = surface->resource;
    SceneReleaseContext ctx;
    ctx.kind = SceneWindow::Kind::Wayland;
    ctx.buffer = buffer;
    ctx.surface = surface->resource;
    g_scene_release_queue[surface->scene_window_id].push_back(ctx);
    // 场景只借用 fence fd 做 poll；所有权归合成器，这里用完即关。
    if (acquire_fd >= 0) ::close(acquire_fd);
    KOP_LOG_DEBUG(kTag, "dmabuf 帧进入场景 %ux%u fmt=0x%x（%s）", view.width,
                  view.height, view.format, surface->title.c_str());
}

void send_initial_configure(Surface* surface) {
    if (!surface || !surface->xdg_surface || !surface->xdg_toplevel || !g_display ||
        surface->configure_serial != 0) {
        return;
    }
    wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(surface->xdg_toplevel, 0, 0, &states);
    wl_array_release(&states);
    surface->configure_serial = wl_display_next_serial(g_display);
    xdg_surface_send_configure(surface->xdg_surface, surface->configure_serial);
    wl_display_flush_clients(g_display);
}

uint64_t g_commit_counter = 1;

void present_pending_buffer(Surface* surface) {
    if (!surface || !surface->pending_buffer) return;
    surface->commit_seq = ++g_commit_counter;
    wl_resource* buffer = surface->pending_buffer;
    if (surface->pending_buffer_listener) {
        wl_list_remove(&surface->pending_buffer_destroy.link);
        wl_list_init(&surface->pending_buffer_destroy.link);
        surface->pending_buffer_listener = false;
    }
    surface->pending_buffer = nullptr;

    if (kopms::DmabufBridge::is_dmabuf_buffer(buffer)) {
        // M4：linux-dmabuf 走 GPU 场景路径，绝不进入 wl_shm 复制路径。
        present_dmabuf_buffer(surface, buffer);
        return;
    }
    // dmabuf 帧不进 M1 嵌套输出；输入聚焦仍以 shm 主 surface 为准。

    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (!shm) {
        // 其余 buffer 类型仍不接受。GPU 帧的合法入口只有 dmabuf wl_buffer
        // 与独立帧桥两条。
        KOP_LOG_WARN(kTag, "拒绝未知 buffer 类型");
        wl_buffer_send_release(buffer);
        return;
    }

    const int32_t width = wl_shm_buffer_get_width(shm);
    const int32_t height = wl_shm_buffer_get_height(shm);
    const int32_t stride = wl_shm_buffer_get_stride(shm);
    const uint32_t format = wl_shm_buffer_get_format(shm);
    if (width <= 0 || height <= 0 || stride < width * 4 ||
        (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)) {
        KOP_LOG_WARN(kTag, "拒绝无效 wl_shm buffer（%dx%d stride=%d fmt=0x%x）", width, height,
                     stride, format);
        wl_buffer_send_release(buffer);
        return;
    }

    wl_shm_buffer_begin_access(shm);
    if (g_output) {
        g_output->present(wl_shm_buffer_get_data(shm), width, height, stride, format);
    }
    wl_shm_buffer_end_access(shm);
    wl_buffer_send_release(buffer);
    KOP_LOG_DEBUG(kTag, "surface commit %dx%d fmt=0x%x（%s）", width, height, format,
                  surface->title.c_str());
    // The nested output copied the pixels before returning. It is now safe to
    // release the client buffer and complete callbacks for this commit.
    send_frame_callbacks(surface, now_millis());
}

// ---------------------------------------------------------------------------
// wl_surface
// ---------------------------------------------------------------------------
void surface_attach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t, int32_t) {
    Surface* surface = surface_from(resource);
    if (!surface || surface->destroyed) return;
    release_pending_buffer(surface);
    if (buffer) track_pending_buffer(surface, buffer);
}

void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_set_buffer_transform(wl_client*, wl_resource*, int32_t) {}
void surface_set_buffer_scale(wl_client*, wl_resource*, int32_t) {}
void surface_damage_buffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t,
                           int32_t) {}

void surface_frame(wl_client* client, wl_resource* resource, uint32_t callback_id) {
    Surface* surface = surface_from(resource);
    if (!surface || surface->destroyed) return;
    auto callback = std::make_unique<FrameCallback>();
    callback->surface = surface;
    callback->resource = wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    if (!callback->resource) return;
    wl_resource_set_implementation(callback->resource, nullptr, callback.get(),
                                   frame_callback_destroy);
    surface->frame_cbs.push_back(std::move(callback));
}

void surface_commit(wl_client*, wl_resource* resource) {
    Surface* surface = surface_from(resource);
    if (!surface || surface->destroyed) return;
    if (!surface->xdg_toplevel || !surface->xdg_surface) {
        KOP_LOG_WARN(kTag, "wl_surface commit 没有 xdg role");
        return;
    }
    if (!surface->configured) {
        // Keep the attached buffer. It is presented by ack_configure, which
        // makes the configure/ack/commit ordering observable in M1 tests.
        send_initial_configure(surface);
        return;
    }
    if (surface->pending_buffer) present_pending_buffer(surface);
    else send_frame_callbacks(surface, now_millis());
}

void surface_res_destroy(wl_resource* resource) {
    Surface* surface = surface_from(resource);
    if (!surface) return;
    surface->destroyed = true;
    release_pending_buffer(surface);
    cancel_frame_callbacks(surface);
    if (surface->resource == resource) surface->resource = nullptr;
    // M4：清理 explicit-sync 状态与场景注册表中的挂起条目。
    g_dmabuf.surface_destroyed(resource);
    if (surface->scene_window_id != 0) {
        g_scene_windows.erase(surface->scene_window_id);
        g_scene_release_queue.erase(surface->scene_window_id);
    }
    KOP_LOG_INFO(kTag, "wl_surface 资源销毁（title=%s）", surface->title.c_str());
    // Keep the C++ object until display teardown: xdg resources can be
    // destroyed after wl_surface and still carry this user-data pointer.
}

void surface_destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
};

void compositor_create_surface(wl_client* client, wl_resource*, uint32_t id) {
    auto surface = std::make_unique<Surface>();
    wl_resource* resource = wl_resource_create(client, &wl_surface_interface, 4, id);
    if (!resource) return;
    surface->resource = resource;
    surface->pending_buffer_destroy.notify = pending_buffer_destroyed;
    wl_list_init(&surface->pending_buffer_destroy.link);
    wl_resource_set_implementation(resource, &surface_impl, surface.get(), surface_res_destroy);
    g_surfaces.push_back(std::move(surface));
    KOP_LOG_INFO(kTag, "wl_surface 创建");
}

void region_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void region_add_req(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {
    // opaque/input region 仅作为合成提示；当前合成器忽略区域语义。
}

const struct wl_region_interface region_impl = {
    .destroy = region_destroy_req,
    .add = region_add_req,
    .subtract = region_add_req,
};

void compositor_create_region(wl_client* client, wl_resource*, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &wl_region_interface, 1, id);
    if (resource) wl_resource_set_implementation(resource, &region_impl, nullptr, nullptr);
}

const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

void compositor_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    const uint32_t bound_version = version > 4 ? 4 : version;
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface, bound_version, id);
    if (resource) wl_resource_set_implementation(resource, &compositor_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// xdg_surface / xdg_toplevel
//
// 真实客户端（GTK/Qt）会发送大量纯状态类请求（set_parent/set_app_id/
// set_min_size 等）。任何 NULL 处理器都会让 libwayland 杀掉连接甚至整个
// 进程——全部请求必须有实现（语义未支持的为显式 no-op）。
// ---------------------------------------------------------------------------
struct ToplevelState {
    Surface* surface = nullptr;
    std::string app_id;
    int32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    bool maximized = false, minimized = false, fullscreen = false;
};

void toplevel_set_title(wl_client*, wl_resource* resource, const char* title) {
    auto* state = static_cast<ToplevelState*>(wl_resource_get_user_data(resource));
    if (state && state->surface) state->surface->title = title ? title : "";
}

void toplevel_set_app_id(wl_client*, wl_resource* resource, const char* app_id) {
    auto* state = static_cast<ToplevelState*>(wl_resource_get_user_data(resource));
    if (state) state->app_id = app_id ? app_id : "";
}

void toplevel_set_parent(wl_client*, wl_resource*, wl_resource* parent) {
    // nil parent = 顶层窗口；父子层级管理属 CONTROL lane 的窗口树。
    (void)parent;
}

void toplevel_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void toplevel_res_destroy(wl_resource* resource) {
    auto* state = static_cast<ToplevelState*>(wl_resource_get_user_data(resource));
    if (state && state->surface && state->surface->xdg_toplevel == resource) {
        state->surface->xdg_toplevel = nullptr;
    }
    delete state;
}

void toplevel_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t,
                               int32_t, int32_t) {}
void toplevel_move(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void toplevel_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
void toplevel_set_max_size(wl_client*, wl_resource* resource, int32_t w, int32_t h) {
    auto* state = static_cast<ToplevelState*>(wl_resource_get_user_data(resource));
    if (state) {
        state->max_w = w;
        state->max_h = h;
    }
}
void toplevel_set_min_size(wl_client*, wl_resource* resource, int32_t w, int32_t h) {
    auto* state = static_cast<ToplevelState*>(wl_resource_get_user_data(resource));
    if (state) {
        state->min_w = w;
        state->min_h = h;
    }
}
void toplevel_set_maximized(wl_client*, wl_resource*) {}
void toplevel_unset_maximized(wl_client*, wl_resource*) {}
void toplevel_set_fullscreen(wl_client*, wl_resource*, wl_resource*) {}
void toplevel_unset_fullscreen(wl_client*, wl_resource*) {}
void toplevel_set_minimized(wl_client*, wl_resource*) {}

const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy_req,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

void xdg_surface_destroy_req(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void xdg_surface_res_destroy(wl_resource* resource) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (surface && surface->xdg_surface == resource) surface->xdg_surface = nullptr;
}

void xdg_surface_get_toplevel(wl_client* client, wl_resource* resource, uint32_t id) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (!surface || surface->xdg_toplevel) return;
    wl_resource* toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
    if (!toplevel) return;
    surface->xdg_toplevel = toplevel;
    auto* state = new ToplevelState();
    state->surface = surface;
    wl_resource_set_implementation(toplevel, &toplevel_impl, state, toplevel_res_destroy);
    send_initial_configure(surface);
    KOP_LOG_INFO(kTag, "xdg_toplevel 创建，已发送 configure");
}

void xdg_surface_ack_configure(wl_client*, wl_resource* resource, uint32_t serial) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (!surface) return;
    if (surface->configure_serial == 0 || serial != surface->configure_serial) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                               "ack_configure serial %u is not outstanding", serial);
        return;
    }
    surface->configure_serial = 0;
    surface->configured = true;
    KOP_LOG_DEBUG(kTag, "ack_configure serial=%u", serial);
    if (surface->pending_buffer && surface->resource) present_pending_buffer(surface);
}

void xdg_surface_set_geom_nop(wl_client*, wl_resource*, int32_t, int32_t, int32_t,
                              int32_t) {}

// xdg_popup：对象必须真实存在（客户端会销毁/抓取）；不注入弹窗语义。
void popup_grab(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void popup_reposition(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
void popup_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy_req,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

void xdg_surface_get_popup(wl_client* client, wl_resource* resource, uint32_t id,
                           wl_resource* parent_surface, wl_resource* positioner) {
    auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
    if (!surface) return;
    wl_resource* popup = wl_resource_create(client, &xdg_popup_interface, 1, id);
    if (!popup) return;
    wl_resource_set_implementation(popup, &popup_impl, nullptr, nullptr);
    (void)parent_surface;
    (void)positioner;
}

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy_req,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_geom_nop,
    .ack_configure = xdg_surface_ack_configure,
};

void positioner_destroy_req(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

// xdg-shell v1 positioner 仅 5 个请求；尺寸/锚点语义由客户端自持。
const struct xdg_positioner_interface positioner_impl = {
    .destroy = positioner_destroy_req,
    .set_size = nullptr,
    .set_anchor_rect = nullptr,
    .set_anchor = nullptr,
    .set_gravity = nullptr,
};

void wm_base_create_positioner(wl_client* client, wl_resource*, uint32_t id) {
    wl_resource* positioner =
        wl_resource_create(client, &xdg_positioner_interface, 1, id);
    if (!positioner) return;
    wl_resource_set_implementation(positioner, &positioner_impl, nullptr,
                                   nullptr);
}

void xdg_wm_base_destroy_req(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void xdg_wm_base_get_xdg_surface(wl_client* client, wl_resource*, uint32_t id,
                                  wl_resource* surface_resource) {
    auto* surface = surface_from(surface_resource);
    if (!surface || surface->xdg_surface) return;
    wl_resource* xdg_surface = wl_resource_create(client, &xdg_surface_interface, 1, id);
    if (!xdg_surface) return;
    surface->xdg_surface = xdg_surface;
    wl_resource_set_implementation(xdg_surface, &xdg_surface_impl, surface,
                                   xdg_surface_res_destroy);
}

void xdg_wm_base_pong(wl_client*, wl_resource*, uint32_t) {}

const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = xdg_wm_base_destroy_req,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

void wm_base_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    wl_resource* resource = wl_resource_create(client, &xdg_wm_base_interface,
                                               version > 1 ? 1 : version, id);
    if (resource) wl_resource_set_implementation(resource, &wm_base_impl, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// seat：真实 wl_pointer/wl_keyboard 资源对象（能力协商必需——客户端请求
// get_pointer/get_keyboard 时必须返回对象，否则后续请求落在未分配 id 上，
// libwayland 会直接断开连接；当前只创建对象、不注入任何输入事件）。
// ---------------------------------------------------------------------------
void pointer_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t,
                        int32_t) {
    // 光标形状由合成器自管（nested 输出用桌面光标）；不注入输入。
}

void pointer_release(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

const struct wl_pointer_interface pointer_impl = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};

void keyboard_release(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

void seat_get_pointer(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* pointer = wl_resource_create(
        client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (pointer) {
        wl_resource_set_implementation(pointer, &pointer_impl, nullptr, nullptr);
        g_input.register_pointer(pointer);
    }
}

void seat_get_keyboard(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* keyboard = wl_resource_create(
        client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (keyboard) {
        wl_resource_set_implementation(keyboard, &keyboard_impl, nullptr, nullptr);
        g_input.register_keyboard(keyboard);
    }
}

void seat_get_touch(wl_client* client, wl_resource* resource, uint32_t id) {
    wl_resource* touch = wl_resource_create(client, &wl_touch_interface,
                                            wl_resource_get_version(resource), id);
    if (touch) {
        wl_resource_set_implementation(touch, nullptr, nullptr, nullptr);
    }
}

void seat_release(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

void seat_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    const uint32_t bound_version = version > 5 ? 5 : version;
    wl_resource* resource = wl_resource_create(client, &wl_seat_interface, bound_version, id);
    if (!resource) return;
    wl_resource_set_implementation(resource, &seat_impl, nullptr, nullptr);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (bound_version >= 2) wl_seat_send_name(resource, "kopms-seat");
}

void output_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    const uint32_t bound_version = version > 2 ? 2 : version;
    wl_resource* resource = wl_resource_create(client, &wl_output_interface, bound_version, id);
    if (!resource) return;
    wl_output_send_geometry(resource, 0, 0, 300, 190, WL_OUTPUT_SUBPIXEL_UNKNOWN, "KOP",
                            "KOPMS-M1", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1280, 720,
                        60000);
    if (bound_version >= 2) wl_output_send_scale(resource, 1);
    if (bound_version >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done(resource);
}

void client_destroyed(wl_listener* listener, void*) {
    ClientInfo* info = wl_container_of(listener, info, destroy);
    KOP_LOG_INFO(kTag, "客户端正常断开 %p", static_cast<void*>(info->client));
    g_clients.remove_if([info](const std::unique_ptr<ClientInfo>& item) {
        return item.get() == info;
    });
}

void client_created(wl_listener*, void* data) {
    auto* client = static_cast<wl_client*>(data);
    auto info = std::make_unique<ClientInfo>();
    info->client = client;
    info->destroy.notify = client_destroyed;
    wl_list_init(&info->destroy.link);
    wl_client_add_destroy_listener(client, &info->destroy);
    g_clients.push_back(std::move(info));
    KOP_LOG_INFO(kTag, "客户端已连接 %p", static_cast<void*>(client));
}

wl_listener g_client_created{};

// ---------------------------------------------------------------------------
// P3-M3/M4：场景布局与 DRM 直出呈现
// ---------------------------------------------------------------------------

// 多窗口 GPU 合成布局：场景窗口网格平铺；CONTROL 焦点窗口最后绘制（置顶）。
// 内容在各自网格单元内 letterbox（保持纵横比）。
std::vector<kopms::VulkanScene::LayoutItem> compute_scene_layout(uint32_t out_w,
                                                                 uint32_t out_h) {
    std::vector<kopms::VulkanScene::LayoutItem> items;
    if (g_scene_windows.empty() || out_w == 0 || out_h == 0) return items;
    const uint64_t focused_control =
        g_bus_server ? g_bus_server->focused_control_window() : 0;
    std::vector<uint64_t> ids;
    ids.reserve(g_scene_windows.size());
    for (const auto& entry : g_scene_windows) ids.push_back(entry.first);
    std::stable_sort(ids.begin(), ids.end());
    const size_t count = ids.size();
    const uint32_t cols =
        static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const uint32_t rows = static_cast<uint32_t>((count + cols - 1) / cols);
    const uint32_t cell_w = out_w / cols;
    const uint32_t cell_h = out_h / rows;
    for (size_t i = 0; i < count; ++i) {
        const SceneWindow& win = g_scene_windows[ids[i]];
        uint32_t w = cell_w;
        uint32_t h = cell_h;
        if (win.content_w != 0 && win.content_h != 0) {
            const double scale =
                std::min(static_cast<double>(cell_w) / win.content_w,
                         static_cast<double>(cell_h) / win.content_h);
            w = static_cast<uint32_t>(win.content_w * scale);
            h = static_cast<uint32_t>(win.content_h * scale);
        }
        kopms::VulkanScene::LayoutItem item;
        item.window_id = ids[i];
        item.rect = {static_cast<int32_t>((i % cols) * cell_w + (cell_w - w) / 2),
                     static_cast<int32_t>((i / cols) * cell_h + (cell_h - h) / 2),
                     w, h};
        item.focused = focused_control != 0 && win.control_id == focused_control;
        item.z = item.focused ? count + 1 : static_cast<uint64_t>(i);
        items.push_back(item);
    }
    return items;
}

void present_scene_to_drm(kopms::DrmDirectOutput& output) {
    int fd = -1;
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string error;
    // export_composite_dmabuf 先等待合成 fence：KMS 扫描前内容已就绪。
    if (!g_scene.export_composite_dmabuf(&fd, &stride, &width, &height, &error)) {
        KOP_LOG_WARN(kTag, "合成结果导出失败：%s", error.c_str());
        g_scene.complete_direct_present();
        return;
    }
    kopms::DrmDmabufImportRequest request{};
    request.media_type = KOPAW_MEDIA_VIDEO;
    request.width = width;
    request.height = height;
    request.format = 0x34325241u;  // AR24 ARGB8888（场景目标为 B8G8R8A8）
    request.plane_count = 1;
    request.planes[0].fd = fd;
    request.planes[0].stride = stride;
    const bool flipped =
        output.present_dmabuf(request, [] { g_scene.complete_direct_present(); },
                              &error);
    ::close(fd);
    if (!flipped) {
        KOP_LOG_WARN(kTag, "DRM page-flip 呈现失败：%s", error.c_str());
        g_scene.complete_direct_present();
    }
}

struct HotplugDispatchContext {
    kopms::DrmHotplugMonitor* monitor = nullptr;
};

int dispatch_hotplug_events(int, uint32_t mask, void* data) {
    auto* context = static_cast<HotplugDispatchContext*>(data);
    if (!context || !context->monitor) return 0;
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
        KOP_LOG_WARN(kTag, "DRM udev monitor event source closed");
        return 0;
    }
    std::string error;
    if (!context->monitor->dispatch(&error)) {
        KOP_LOG_WARN(kTag, "DRM udev event 处理停止：%s", error.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    kop::log_set_level(kop::log_level_from_env());
    RuntimeOptions options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argc > 0 ? argv[0] : nullptr);
        return 2;
    }
    if (!options.gpu_mode_diagnostic.empty()) {
        KOP_LOG_WARN(kTag, "%s，回退到 SINGLE", options.gpu_mode_diagnostic.c_str());
    }
    KOP_LOG_INFO(kTag, "KOPMS GPU handle mode=%s", kopms::gpu_mode_name(options.gpu_mode));

    g_display = wl_display_create();
    if (!g_display) {
        KOP_LOG_ERROR(kTag, "wl_display_create 失败");
        return 1;
    }
    if (wl_display_add_socket(g_display, options.socket_name.c_str()) < 0) {
        KOP_LOG_ERROR(kTag, "创建 socket %s 失败", options.socket_name.c_str());
        wl_display_destroy(g_display);
        g_display = nullptr;
        return 1;
    }
    if (wl_display_init_shm(g_display) < 0) {
        KOP_LOG_ERROR(kTag, "wl_display_init_shm 失败");
        wl_display_destroy(g_display);
        g_display = nullptr;
        return 1;
    }
    wl_global_create(g_display, &wl_compositor_interface, 4, nullptr, compositor_bind);
    // wl_shm is published by wl_display_init_shm; do not publish it twice.
    wl_global_create(g_display, &wl_seat_interface, 5, nullptr, seat_bind);
    wl_global_create(g_display, &wl_output_interface, 2, nullptr, output_bind);
    wl_global_create(g_display, &xdg_wm_base_interface, 1, nullptr, wm_base_bind);
    g_client_created.notify = client_created;
    wl_list_init(&g_client_created.link);
    wl_display_add_client_created_listener(g_display, &g_client_created);

    kopms::DrmKmsGuard drm(options.drm_device);
    if (options.probe_drm) {
        kopms::DrmKmsSnapshot snapshot;
        std::string error;
        if (drm.discover(&snapshot, &error)) {
            KOP_LOG_INFO(kTag,
                         "DRM/KMS 资源发现成功：driver=%s kms=%s atomic=%s "
                         "universal_planes=%s connectors=%zu crtcs=%zu planes=%zu",
                         snapshot.driver_name.c_str(), snapshot.kms ? "yes" : "no",
                         snapshot.atomic ? "yes" : "no",
                         snapshot.universal_planes ? "yes" : "no",
                         snapshot.connectors.size(), snapshot.crtcs.size(),
                         snapshot.planes.size());
            KOP_LOG_INFO(kTag, "DRM/KMS atomic properties=%s",
                         snapshot.properties.modeset_ready() ? "ready" : "incomplete");
            if (snapshot.selection.valid) {
                KOP_LOG_INFO(kTag,
                             "DRM/KMS 输出候选：connector=%u encoder=%u crtc=%u "
                             "primary_plane=%u mode=%s (%ux%u@%uHz)",
                             snapshot.selection.connector_id, snapshot.selection.encoder_id,
                             snapshot.selection.crtc_id,
                             snapshot.selection.primary_plane_id,
                             snapshot.selection.mode.name.c_str(),
                             snapshot.selection.mode.width, snapshot.selection.mode.height,
                             snapshot.selection.mode.refresh_hz);
            } else {
                KOP_LOG_WARN(kTag,
                             "DRM/KMS 设备可访问但没有完整输出候选：%s；当前仍使用 nested 输出",
                             snapshot.selection_error.empty()
                                 ? "unknown reason"
                                 : snapshot.selection_error.c_str());
            }
        } else {
            KOP_LOG_WARN(kTag, "DRM/KMS 探测跳过：%s", error.c_str());
        }
    }

    kopms::LibinputGuard input;
    if (options.enable_input) {
        std::string error;
        if (!input.start(options.seat, &error)) {
            KOP_LOG_WARN(kTag, "libinput 启动跳过：%s", error.c_str());
        } else {
            KOP_LOG_INFO(kTag, "libinput 防护已启用（seat=%s，输入注入仍禁用）",
                         options.seat.c_str());
        }
    }

    NestedOutput output;
    bool nested_initialized = false;
    auto enable_nested_output = [&]() -> bool {
        if (nested_initialized) {
            g_output = &output;
            return true;
        }
        if (!output.init(1280, 720, "KOPMS M1")) {
            KOP_LOG_WARN(kTag, "nested 输出初始化失败，当前没有可用显示输出");
            return false;
        }
        output.set_input_callbacks({
            .cursor = [](double x, double y) { g_input.cursor_pos(x, y); },
            .mouse_button = [](int b, int a) { g_input.mouse_button(b, a); },
            .key = [](int k, int sc, int ac, int m) {
                g_input.keyboard_key(k, sc, ac, m);
            },
        });
        nested_initialized = true;
        g_output = &output;
        KOP_LOG_INFO(kTag, "KOPMS nested 输出已启用");
        return true;
    };
    auto disable_nested_output = [&]() {
        g_output = nullptr;
        if (nested_initialized) {
            output.shutdown();
            nested_initialized = false;
        }
    };

    kopms::DrmDirectOutput direct_output;
    auto direct_state_changed = [&](kopms::DrmOutputState state,
                                    const std::string& reason) {
        KOP_LOG_INFO(kTag, "DRM 物理输出状态=%s：%s",
                     kopms::drm_output_state_name(state), reason.c_str());
        if (state == kopms::DrmOutputState::Active) {
            disable_nested_output();
            // M3：直出模式下场景释放由 page-flip 完成事件驱动。
            g_scene.set_direct_mode(true);
        } else if (state == kopms::DrmOutputState::Paused ||
                   state == kopms::DrmOutputState::Revoked ||
                   state == kopms::DrmOutputState::Failed) {
            enable_nested_output();
            g_scene.set_direct_mode(false);
        }
    };

    kopms::SeatSession seat_session(options.seat, options.allow_unmanaged_drm);
    bool direct_requested = options.direct_drm;
    bool seat_session_started = false;
    if (direct_requested) {
        std::string seat_error;
        const bool seat_started = seat_session.start(
            [&](kopms::SeatSessionState state, const std::string& reason) {
                // The first Active notification is emitted by SeatSession
                // before DrmDirectOutput has bound its device. Activation is
                // performed explicitly just below.
                if (direct_output.device().empty()) return;
                std::string output_error;
                if (!direct_output.handle_seat_state(state, reason, &output_error)) {
                    KOP_LOG_WARN(kTag, "DRM seat 状态处理失败：%s", output_error.c_str());
                }
            },
            &seat_error);
        if (!seat_started) {
            KOP_LOG_WARN(kTag, "DRM 物理直出未启用：%s；回退 nested", seat_error.c_str());
        } else {
            seat_session_started = true;
            std::string output_error;
            if (!direct_output.start(&seat_session, options.drm_device,
                                     direct_state_changed, &output_error)) {
                KOP_LOG_WARN(kTag, "DRM 物理直出启动失败：%s；回退 nested",
                             output_error.c_str());
                direct_output.stop();
                seat_session.stop();
                seat_session_started = false;
            } else {
                KOP_LOG_INFO(kTag, "DRM 物理直出已通过 seat、资源和 atomic TEST_ONLY");
            }
        }
    }

    if (!direct_output.active() && !enable_nested_output()) {
        KOP_LOG_ERROR(kTag, "nested/DRM 均无法提供显示输出");
        direct_output.stop();
        seat_session.stop();
        wl_display_destroy(g_display);
        g_display = nullptr;
        return 1;
    }

    wl_event_loop* loop = wl_display_get_event_loop(g_display);

    // P3 输入管线：指针落点解析（最近提交内容的 xdg 主 surface，1:1 内容矩形）
    g_input.init(g_display, loop,
                 {[](int win_x, int win_y, wl_resource** surface, int32_t* sx,
                     int32_t* sy) {
                     Surface* best = nullptr;
                     for (const auto& item : g_surfaces) {
                         if (item->destroyed || item->commit_seq == 0) continue;
                         if (!item->xdg_toplevel || !item->resource) continue;
                         if (!best || item->commit_seq > best->commit_seq) {
                             best = item.get();
                         }
                     }
                     if (!best) return false;
                     const auto rect = g_output ? g_output->content_rect()
                                                : kopms::NestedOutput::ContentRect{};
                     if (rect.w == 0 || rect.h == 0) return false;
                     if (win_x < rect.x || win_y < rect.y ||
                         win_x >= rect.x + rect.w || win_y >= rect.y + rect.h) {
                         KOP_LOG_DEBUG("kopms-input", "target miss (%d,%d) rect=(%d,%d %dx%d)",
                                       win_x, win_y, rect.x, rect.y, rect.w, rect.h);
                         return false;
                     }
                     *surface = best->resource;
                     *sx = win_x - rect.x;
                     *sy = win_y - rect.y;
                     return true;
                 },
                 []() {
                     for (const auto& item : g_surfaces) {
                         if (!item->destroyed && item->commit_seq != 0 &&
                             item->xdg_toplevel && item->resource) {
                             return true;
                         }
                     }
                     return false;
                 },
                 []() {
                     if (!g_output) return kopms::InputRect{};
                     const auto rect = g_output->content_rect();
                     return kopms::InputRect{rect.x, rect.y, rect.w, rect.h};
                 }});
    // 持久压测自驱动：KOPMS_INPUT_SOAK="rounds[,interval_ms]"
    if (const char* soak = std::getenv("KOPMS_INPUT_SOAK"); soak && soak[0] != '\0') {
        unsigned long rounds = strtoul(soak, nullptr, 10);
        unsigned long interval = 120;
        if (const char* comma = strchr(soak, ','); comma) {
            interval = strtoul(comma + 1, nullptr, 10);
        }
        if (rounds > 0) g_input.start_soak(static_cast<uint32_t>(rounds),
                                           static_cast<uint32_t>(interval));
    }

    kopms::DrmHotplugMonitor hotplug;
    HotplugDispatchContext hotplug_context{&hotplug};
    wl_event_source* hotplug_source = nullptr;
    if (direct_output.active() && options.watch_drm) {
        std::string hotplug_error;
        if (!hotplug.start(
                options.drm_device,
                [&](kopms::DrmHotplugAction action, const std::string& device) {
                    std::string output_error;
                    if (!direct_output.handle_hotplug(action, device, &output_error)) {
                        KOP_LOG_WARN(kTag, "DRM 热插拔恢复失败（%s）：%s",
                                     kopms::drm_hotplug_action_name(action),
                                     output_error.c_str());
                    }
                },
                &hotplug_error)) {
            KOP_LOG_WARN(kTag, "DRM 热插拔监听未启用：%s", hotplug_error.c_str());
        } else {
            hotplug_source = wl_event_loop_add_fd(
                loop, hotplug.fd(), WL_EVENT_READABLE, dispatch_hotplug_events,
                &hotplug_context);
            if (!hotplug_source) {
                KOP_LOG_WARN(kTag, "DRM 热插拔 fd 无法加入 Wayland event loop");
                hotplug.stop();
            } else {
                KOP_LOG_INFO(kTag, "DRM 热插拔监听已启用");
            }
        }
    }
    // P3-M3/M4：Vulkan 场景（DMA-BUF 导入 + 多窗口合成）。direct 模式无
    // nested 输出时场景保持离屏；--scene-window 才创建独立呈现窗口。
    GLFWwindow* scene_window = nullptr;
    if (options.scene_window && !direct_output.active()) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        scene_window = glfwCreateWindow(1280, 720, "KOPMS Scene", nullptr, nullptr);
        if (!scene_window) {
            KOP_LOG_WARN(kTag, "scene 呈现窗口创建失败，回退离屏合成");
        }
    }
    kopms::VulkanScene::Options scene_options{};
    scene_options.windowed = scene_window != nullptr;
    scene_options.width = 1280;
    scene_options.height = 720;
    std::string scene_error;
    if (g_scene.init(scene_options, scene_window, &scene_error)) {
        KOP_LOG_WARN(kTag, "Vulkan 场景未启用：%s；BUS/Wayland GPU 帧将被立即释放",
                     scene_error.c_str());
    } else {
        g_scene.set_release_callback(on_scene_release);
        KOP_LOG_INFO(kTag, "Vulkan 场景合成已启用（%s）",
                     scene_options.windowed ? "windowed" : "offscreen");
    }

    // P3-M4：linux-dmabuf / explicit-sync 全局（协议 XML 可用时）。
    if (!g_dmabuf.install(g_display, &scene_error)) {
        KOP_LOG_WARN(kTag, "linux-dmabuf 全局安装失败：%s", scene_error.c_str());
    }

    kopms::KopmsServer bus_server(
        loop, options.bus_socket_name,
        [&](kopms::KopmsReceivedFrame& frame) -> kopms::FrameDisposition {
            // M3：KOPAW 帧（KopmsFrameDescriptor）→ Vulkan 场景。帧被拒收时
            // 立即以 DROPPED 释放，绝不走 wl_shm 路径。
            if (!g_scene.inited()) return kopms::FrameDisposition::Release;
            uint64_t window_id = 0;
            for (const auto& info : bus_server.control_snapshot()) {
                if (info.attached_session == frame.session_id) {
                    window_id = info.id;
                    break;
                }
            }
            if (window_id == 0) {
                window_id = kImplicitSceneWindowBase + frame.session_id;
            }
            kopms::VulkanScene::ImportRequest request{};
            request.width = frame.payload.width;
            request.height = frame.payload.height;
            request.format = frame.payload.format;
            request.plane_count = frame.payload.plane_count;
            int plane_fds[KOPAW_MAX_DMABUF_PLANES] = {-1, -1, -1, -1};
            uint32_t offsets[KOPAW_MAX_DMABUF_PLANES] = {};
            uint32_t strides[KOPAW_MAX_DMABUF_PLANES] = {};
            uint64_t modifiers[KOPAW_MAX_DMABUF_PLANES] = {};
            for (uint32_t i = 0; i < frame.payload.plane_count &&
                                 i < KOPAW_MAX_DMABUF_PLANES;
                 ++i) {
                const int32_t index = frame.payload.planes[i].fd_index;
                if (index < 0 ||
                    static_cast<size_t>(index) >= frame.fds.size()) {
                    return kopms::FrameDisposition::ReleaseDropped;
                }
                plane_fds[i] = frame.fds[static_cast<size_t>(index)];
                offsets[i] = frame.payload.planes[i].offset;
                strides[i] = frame.payload.planes[i].stride;
                modifiers[i] = frame.payload.planes[i].modifier;
            }
            int fence_fd = -1;
            if (frame.payload.acquire_fence.kind == KOPAW_SYNC_FENCE_FD) {
                const int32_t index = frame.payload.acquire_fence.fd_index;
                if (index < 0 || static_cast<size_t>(index) >= frame.fds.size()) {
                    return kopms::FrameDisposition::ReleaseDropped;
                }
                fence_fd = frame.fds[static_cast<size_t>(index)];
            }
            request.plane_fds = plane_fds;
            request.plane_offsets = offsets;
            request.plane_strides = strides;
            request.plane_modifiers = modifiers;
            request.acquire_fence_kind = fence_fd >= 0 ? KOPAW_SYNC_FENCE_FD
                                                       : KOPAW_SYNC_FENCE_NONE;
            request.acquire_fence_fd = fence_fd;
            request.color = frame.payload.color;
            request.has_color_metadata =
                frame.payload.struct_size >= KOPMS_FRAME_SUBMIT_PAYLOAD_SIZE;
            request.session_id = frame.session_id;
            request.frame_id = frame.payload.frame_id;

            std::string error;
            const bool was_backpressured = g_scene.backpressured(window_id);
            if (!g_scene.submit(window_id, request, &error)) {
                KOP_LOG_DEBUG(kTag,
                              "KOPMS-C session=%llu frame=%u 拒收（%s）",
                              static_cast<unsigned long long>(frame.session_id),
                              frame.payload.frame_id, error.c_str());
                return was_backpressured ? kopms::FrameDisposition::ReleaseDropped
                                         : kopms::FrameDisposition::ReleaseRejected;
            }
            if (getenv("KOPMS_RELEASE_DEBUG")) {
                KOP_LOG_DEBUG(kTag, "handler frame=%u → window=%llu Retain",
                              frame.payload.frame_id,
                              static_cast<unsigned long long>(window_id));
            }
            SceneWindow& win = g_scene_windows[window_id];
            win.kind = SceneWindow::Kind::Bus;
            win.content_w = frame.payload.width;
            win.content_h = frame.payload.height;
            win.session = frame.session_id;
            win.frame_id = frame.payload.frame_id;
            win.control_id =
                window_id < kImplicitSceneWindowBase ? window_id : 0;
            SceneReleaseContext ctx;
            ctx.kind = SceneWindow::Kind::Bus;
            ctx.session = frame.session_id;
            ctx.frame_id = frame.payload.frame_id;
            g_scene_release_queue[window_id].push_back(ctx);
            return kopms::FrameDisposition::Retain;
        },
        options.gpu_mode);
    g_bus_server = &bus_server;
    std::string bus_error;
    if (!bus_server.start(&bus_error)) {
        KOP_LOG_ERROR(kTag, "KOPMS-S BUS2LAYER 启动失败：%s", bus_error.c_str());
        if (hotplug_source) wl_event_source_remove(hotplug_source);
        hotplug.stop();
        direct_output.stop();
        seat_session.stop();
        disable_nested_output();
        g_bus_server = nullptr;
        g_scene_windows.clear();
        g_scene.shutdown();
        wl_display_destroy(g_display);
        g_display = nullptr;
        return 1;
    }
    KOP_LOG_INFO(kTag, "KOPMS 运行中：Wayland=%s BUS=%s output=%s", options.socket_name.c_str(),
                 bus_server.socket_path().c_str(),
                 direct_output.active() ? "direct-drm" : "nested");

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    std::string input_error;
    while (std::chrono::steady_clock::now() < end &&
           !(nested_initialized && output.should_close())) {
        wl_display_flush_clients(g_display);
        if (wl_event_loop_dispatch(loop, 5) < 0) {
            KOP_LOG_ERROR(kTag, "Wayland event loop dispatch 失败");
            break;
        }
        if (direct_requested && seat_session_started &&
            seat_session.state() != kopms::SeatSessionState::Stopped &&
            !seat_session.dispatch(&input_error)) {
            KOP_LOG_WARN(kTag, "seat session 停止：%s", input_error.c_str());
            direct_output.handle_runtime_failure(input_error);
            direct_output.stop();
            seat_session.stop();
            seat_session_started = false;
        }
        if (input.active() && !input.dispatch(&input_error)) {
            KOP_LOG_WARN(kTag, "libinput 停止：%s", input_error.c_str());
            input.stop();
        }
        if (nested_initialized) output.render_frame();
        if (g_scene.inited()) {
            // M3/M4：BUS 帧 + Wayland dmabuf 的多窗口 GPU 合成。
            auto layout = compute_scene_layout(scene_options.width,
                                               scene_options.height);
            if (!g_scene.render(layout, &scene_error)) {
                KOP_LOG_WARN(kTag, "场景合成失败：%s", scene_error.c_str());
            }
            if (g_scene.direct_mode() && direct_output.active()) {
                present_scene_to_drm(direct_output);
            }
        }
    }

    KOP_LOG_INFO(kTag, "正常退出");
    g_bus_server = nullptr;
    bus_server.stop();
    if (hotplug_source) wl_event_source_remove(hotplug_source);
    hotplug.stop();
    direct_output.stop();
    seat_session.stop();
    disable_nested_output();
    g_scene_windows.clear();
    g_scene_release_queue.clear();
    g_scene.shutdown();
    g_input.shutdown();
    wl_display_terminate(g_display);
    wl_display_destroy(g_display);
    g_display = nullptr;
    g_clients.clear();
    g_surfaces.clear();
    return 0;
}
