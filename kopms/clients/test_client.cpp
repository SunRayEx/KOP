// KOPMS M1 测试客户端：连接合成器 → xdg_toplevel 握手 → SHM 动画图案提交循环。
// 用法: WAYLAND_DISPLAY=kop-0 kopms-test-client [秒数=5]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "wayland-client.h"
#include "xdg-shell-client-protocol.h"

namespace {

const int kW = 800;
const int kH = 450;

struct Client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdg_sfc = nullptr;
    xdg_toplevel* top = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    int configured = 0;
    uint64_t frames = 0;
};

void handle_wm_ping(void* data, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
    (void)data;
}

const xdg_wm_base_listener wm_base_listener = {
    .ping = handle_wm_ping,
};

void handle_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    auto* c = static_cast<Client*>(data);
    xdg_surface_ack_configure(c->xdg_sfc, serial);
    c->configured = 1;
}

const xdg_surface_listener kXdgSurfaceListener = {
    .configure = handle_xdg_configure,
};

void handle_toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void handle_toplevel_close(void*, xdg_toplevel*) {}

const xdg_toplevel_listener kToplevelListener = {
    .configure = handle_toplevel_configure,
    .close = handle_toplevel_close,
};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* c = static_cast<Client*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        c->shm = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        c->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

void handle_frame_done(void* data, wl_callback*, uint32_t) {
    auto* c = static_cast<Client*>(data);
    c->frames++;
}

const wl_callback_listener frame_listener = {
    .done = handle_frame_done,
};

void draw_pattern(Client* c, uint64_t frame) {
    const int bar = static_cast<int>(frame * 7 % kW);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            uint32_t r = (x * 255) / kW;
            uint32_t g = (y * 255) / kH;
            uint32_t b = 120;
            if (x >= bar && x < bar + 40) b = 255;  // 移动亮条
            // ARGB8888（小端 = BGRA）
            c->pixels[static_cast<size_t>(y) * kW + x] =
                0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 5;
    Client c{};

    c.display = wl_display_connect(nullptr);
    if (!c.display) {
        fprintf(stderr, "无法连接 Wayland 显示（检查 WAYLAND_DISPLAY）\n");
        return 1;
    }
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    wl_display_roundtrip(c.display);
    if (!c.compositor || !c.shm || !c.wm_base) {
        fprintf(stderr, "缺少必要全局（compositor/shm/xdg_wm_base）\n");
        return 1;
    }

    c.surface = wl_compositor_create_surface(c.compositor);
    c.xdg_sfc = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
    xdg_surface_add_listener(c.xdg_sfc, &kXdgSurfaceListener, &c);
    c.top = xdg_surface_get_toplevel(c.xdg_sfc);
    xdg_toplevel_add_listener(c.top, &kToplevelListener, &c);
    xdg_toplevel_set_title(c.top, "KOPMS test client");
    wl_surface_commit(c.surface);
    // 等 configure 事件并 ack（listener 内处理）
    while (!c.configured) {
        wl_display_dispatch(c.display);
    }
    wl_display_roundtrip(c.display);

    // SHM 池（memfd）
    const size_t size = static_cast<size_t>(kW) * kH * 4;
    int fd = memfd_create("kopms-client", 0);
    ftruncate(fd, static_cast<off_t>(size));
    c.pixels = static_cast<uint32_t*>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, static_cast<int32_t>(size));
    c.buffer = wl_shm_pool_create_buffer(pool, 0, kW, kH, kW * 4,
                                         WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    fprintf(stderr, "test-client: 握手完成，开始提交动画帧\n");
    const auto t_end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < t_end) {
        draw_pattern(&c, c.frames);
        wl_surface_attach(c.surface, c.buffer, 0, 0);
        wl_surface_damage(c.surface, 0, 0, kW, kH);
        wl_callback* cb = wl_surface_frame(c.surface);
        wl_callback_add_listener(cb, &frame_listener, &c);
        wl_surface_commit(c.surface);
        wl_display_flush(c.display);
        // 等待本帧的 frame callback；设 500ms 上限——callback 停滞或
        // 合成器退出（EPIPE）都不再永久阻塞，超时后按窗口结束收尾
        const uint64_t before = c.frames;
        const auto wait_end = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(500);
        while (c.frames == before && std::chrono::steady_clock::now() < wait_end) {
            if (wl_display_dispatch(c.display) < 0) {
                // 合成器关闭连接（正常关停或超时退出都会出现 EPIPE）
                fprintf(stderr,
                        "test-client: 连接关闭（errno=%d），已提交 %lu 帧\n",
                        errno, static_cast<unsigned long>(c.frames));
                return 0;
            }
            wl_display_flush(c.display);
        }
    }
    fprintf(stderr, "test-client: 提交 %lu 帧，正常退出\n",
           static_cast<unsigned long>(c.frames));
    return 0;
}
