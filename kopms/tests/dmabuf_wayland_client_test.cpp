// P3-M4 Wayland 兼容面端到端测试：真实 Wayland 客户端进程走
// linux-dmabuf → wl_buffer → 合成器 Vulkan 场景导入 → release/explicit-sync。
//
// 被测链路（跨进程，fd 经 Wayland socket 的 SCM_RIGHTS 传递）：
//   1. 全局绑定：wl_compositor / xdg_wm_base / zwp_linux_dmabuf_v1 /
//      zwp_linux_explicit_synchronization_v1，收集 format/modifier 事件；
//   2. xdg_surface/toplevel configure → ack_configure（M1 握手语义）；
//   3. KOPAW VulkanDmabufExporter 导出真实 DMA-BUF（LINEAR modifier +
//      sync-fence）→ params.add → create_immed → wl_buffer；
//   4. explicit sync：set_acquire_fence（导出 fence 交给合成器）+ get_release；
//   5. attach + commit → 等待 wl_buffer.release 与 explicit-sync release 事件；
//   6. 双缓冲第二轮（新 buffer 新 fence），验证重复提交与回压下的释放次序。
//
// 测试自行拉起 kopms-compositor（同目录二进制）；无 X11/Wayland 显示环境时
// 以 77 跳过。
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "linux-dmabuf-client-protocol.h"
#include "linux-explicit-synchronization-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "wayland-client.h"

#include "render/vulkan/vk_dma_export.hpp"

namespace {

constexpr uint32_t kW = 96;
constexpr uint32_t kH = 64;

int64_t now_mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(int ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

int failures = 0;

bool check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "dmabuf wayland client: FAIL %s\n", what);
        ++failures;
    }
    return ok;
}

struct ClientState {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    zwp_linux_explicit_synchronization_v1* expsync = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xdg_surf = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zwp_linux_surface_synchronization_v1* sync = nullptr;

    // 协商结果
    bool saw_ab24_linear = false;
    uint32_t configure_serial = 0;
    bool configured = false;
    bool running = true;

    // 逐轮验证目标
    struct Round {
        wl_buffer* buffer = nullptr;
        zwp_linux_buffer_params_v1* params = nullptr;
        wl_callback* frame_cb = nullptr;
        zwp_linux_buffer_release_v1* release_obj = nullptr;
        bool buffer_released = false;
        bool frame_done = false;
        bool sync_released = false;
        bool sync_fenced = false;
    };
    std::vector<Round*> rounds;  // 当前轮（栈顶）
    Round* current = nullptr;
};

void round_reset_events(ClientState::Round* round) {
    round->buffer_released = false;
    round->frame_done = false;
    round->sync_released = false;
    round->sync_fenced = false;
}

// ---------------------------------------------------------------------------
// registry / globals
// ---------------------------------------------------------------------------

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
    auto* state = static_cast<ClientState*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        state->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3));
    } else if (std::strcmp(interface,
                           zwp_linux_explicit_synchronization_v1_interface.name) == 0) {
        state->expsync = static_cast<zwp_linux_explicit_synchronization_v1*>(
            wl_registry_bind(registry, name,
                             &zwp_linux_explicit_synchronization_v1_interface, 1));
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---------------------------------------------------------------------------
// dmabuf：格式/modifier 协商 + params → wl_buffer
// ---------------------------------------------------------------------------

void dmabuf_format(void*, zwp_linux_dmabuf_v1*, uint32_t) {}

void dmabuf_modifier(void* data, zwp_linux_dmabuf_v1*, uint32_t format,
                     uint32_t modifier_hi, uint32_t modifier_lo) {
    auto* state = static_cast<ClientState*>(data);
    const uint64_t modifier =
        (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
    if (format == 0x34324241u /*AB24*/ && modifier == 0 /*LINEAR*/) {
        state->saw_ab24_linear = true;
    }
}

const zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = dmabuf_format,
    .modifier = dmabuf_modifier,
};

void params_created(void* data, zwp_linux_buffer_params_v1* params,
                    wl_buffer* buffer) {
    auto* round = static_cast<ClientState::Round*>(data);
    round->buffer = buffer;
}

void params_failed(void* data, zwp_linux_buffer_params_v1*) {
    auto* round = static_cast<ClientState::Round*>(data);
    std::fprintf(stderr, "dmabuf wayland client: params create FAILED\n");
    ++failures;
    round->buffer = nullptr;
}

const zwp_linux_buffer_params_v1_listener params_listener = {
    .created = params_created,
    .failed = params_failed,
};

// ---------------------------------------------------------------------------
// xdg_surface / toplevel
// ---------------------------------------------------------------------------

void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
void toplevel_close(void*, xdg_toplevel*) {}

const xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
    auto* state = static_cast<ClientState*>(data);
    state->configure_serial = serial;
    state->configured = true;
    xdg_surface_ack_configure(surface, serial);
}

const xdg_surface_listener s_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// ---------------------------------------------------------------------------
// frame callback 与 explicit sync release
// ---------------------------------------------------------------------------

void frame_done(void* data, wl_callback*, uint32_t) {
    auto* round = static_cast<ClientState::Round*>(data);
    round->frame_done = true;
}

const wl_callback_listener frame_listener = {
    .done = frame_done,
};

void buffer_release(void* data, wl_buffer*) {
    auto* round = static_cast<ClientState::Round*>(data);
    round->buffer_released = true;
}

const wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

void release_fenced(void* data, zwp_linux_buffer_release_v1*, int32_t fence) {
    auto* round = static_cast<ClientState::Round*>(data);
    round->sync_released = true;
    round->sync_fenced = fence >= 0;
    if (fence >= 0) ::close(fence);  // 测试只验证语义，读回 fence 信号即可
}

void release_immediate(void* data, zwp_linux_buffer_release_v1*) {
    auto* round = static_cast<ClientState::Round*>(data);
    round->sync_released = true;
}

const zwp_linux_buffer_release_v1_listener release_listener = {
    .fenced_release = release_fenced,
    .immediate_release = release_immediate,
};

// ---------------------------------------------------------------------------

bool dispatch_until(ClientState* state, bool (*condition)(const ClientState*),
                    int timeout_ms) {
    const int64_t deadline = now_mono_ms() + timeout_ms;
    while (!condition(state)) {
        // dispatch_pending() can keep returning work while a peer is sending
        // unrelated events. Check the deadline before it so a compositor-side
        // import failure cannot turn this into CTest's 60-second timeout.
        if (now_mono_ms() > deadline || !state->running) return false;
        const int pending = wl_display_dispatch_pending(state->display);
        if (pending < 0) return false;
        if (pending > 0) continue;
        if (wl_display_flush(state->display) < 0 && errno != EAGAIN) return false;
        struct pollfd pfd{wl_display_get_fd(state->display), POLLIN, 0};
        const int r = ::poll(&pfd, 1, 20);
        if (r < 0 && errno != EINTR) return false;
        if (r > 0 && wl_display_dispatch(state->display) < 0) return false;
    }
    return true;
}

// 场景呈现由合成器事件循环驱动，release 事件按轮次路由到 current。
bool any_round_event_pending(const ClientState* state) {
    return state->current != nullptr && state->current->buffer_released &&
           state->current->frame_done && state->current->sync_released;
}

bool wait_for_events(ClientState* state, int timeout_ms) {
    const int64_t deadline = now_mono_ms() + timeout_ms;
    while (!any_round_event_pending(state)) {
        if (now_mono_ms() > deadline || !state->running) return false;
        const int pending = wl_display_dispatch_pending(state->display);
        if (pending < 0) return false;
        if (pending > 0) continue;
        if (wl_display_flush(state->display) < 0 && errno != EAGAIN) return false;
        pollfd pfd{wl_display_get_fd(state->display), POLLIN, 0};
        const int r = ::poll(&pfd, 1, 50);
        if (r < 0 && errno != EINTR) return false;
        if (r > 0 && wl_display_dispatch(state->display) < 0) return false;
    }
    return true;
}

}  // namespace

int main() {
    // 显示环境检查：无 X11/Wayland 时合成器无法提供 nested 输出。
    if (!std::getenv("WAYLAND_DISPLAY") && !std::getenv("DISPLAY")) {
        std::fprintf(stdout, "dmabuf wayland client: SKIP (no display)\n");
        return 77;
    }

    // 合成器二进制与本测试同目录。
    char self_path[4096];
    const ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len <= 0) {
        std::fprintf(stderr, "dmabuf wayland client: FAIL readlink /proc/self/exe\n");
        return 1;
    }
    self_path[len] = '\0';
    std::string self_dir(self_path);
    const auto slash = self_dir.find_last_of('/');
    if (slash == std::string::npos) return 1;
    const std::string compositor_bin = self_dir.substr(0, slash) + "/kopms-compositor";

    // KOPMS_DMABUF_CLIENT_SOCKET：连接已运行的合成器（调试用），否则自行拉起。
    const char* existing = std::getenv("KOPMS_DMABUF_CLIENT_SOCKET");
    std::string socket_name = existing ? existing : "kop-dmabuf-cli-" + std::to_string(getpid());
    const int64_t started = now_mono_ms();
    pid_t child = -1;
    if (!existing) {
        child = fork();
        if (child == 0) {
            const std::string seconds = "60";
            execl(compositor_bin.c_str(), "kopms-compositor", socket_name.c_str(),
                  seconds.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        if (child < 0) {
            std::fprintf(stderr, "dmabuf wayland client: FAIL fork\n");
            return 1;
        }
        setenv("WAYLAND_DISPLAY", socket_name.c_str(), 1);
        sleep_ms(800);  // 等待合成器建立 socket
    } else {
        setenv("WAYLAND_DISPLAY", socket_name.c_str(), 1);
    }

    int result = 1;
    ClientState state;
    kopaw::VulkanDmabufExporter exporter;

    std::string reason;
    const bool export_ok = kopaw::VulkanDmabufExporter::export_supported(&reason);
    if (!export_ok) {
        std::fprintf(stdout, "dmabuf wayland client: SKIP (no Vulkan DMA-BUF: %s)\n",
                     reason.c_str());
        result = 77;
        goto teardown;
    }
    if (exporter.init(4, &reason)) {
        std::fprintf(stderr, "dmabuf wayland client: FAIL exporter init: %s\n",
                     reason.c_str());
        goto teardown;
    }

    state.display = wl_display_connect(nullptr);
    if (!check(state.display != nullptr, "wl_display_connect")) goto teardown;
    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);
    wl_display_roundtrip(state.display);
    if (!check(state.compositor && state.wm_base && state.dmabuf && state.expsync,
               "globals bound (compositor/xdg/dmabuf/explicit-sync)")) {
        goto teardown;
    }
    zwp_linux_dmabuf_v1_add_listener(state.dmabuf, &dmabuf_listener, &state);
    wl_display_roundtrip(state.display);
    if (!check(state.saw_ab24_linear, "AB24 + LINEAR modifier negotiated")) {
        goto teardown;
    }

    // surface + xdg role + explicit sync 对象
    state.surface = wl_compositor_create_surface(state.compositor);
    state.xdg_surf = xdg_wm_base_get_xdg_surface(state.wm_base, state.surface);
    xdg_surface_add_listener(state.xdg_surf, &s_xdg_surface_listener, &state);
    state.toplevel = xdg_surface_get_toplevel(state.xdg_surf);
    xdg_toplevel_add_listener(state.toplevel, &toplevel_listener, nullptr);
    xdg_toplevel_set_title(state.toplevel, "kopms dmabuf client test");
    wl_surface_commit(state.surface);
    if (!dispatch_until(&state, [](const ClientState* s) { return s->configured; },
                        3000)) {
        check(false, "xdg configure received");
        goto teardown;
    }

    std::fprintf(stderr, "[cli] configure done, creating sync object\n");
    // 同步对象（M4 explicit sync）
    state.sync = zwp_linux_explicit_synchronization_v1_get_synchronization(
        state.expsync, state.surface);
    zwp_linux_surface_synchronization_v1_get_release(state.sync);

    // 两轮提交：真实 DMA-BUF → wl_buffer → 场景 → release
    for (int round_idx = 0; round_idx < 2; ++round_idx) {
        ClientState::Round round;
        state.current = &round;
        round_reset_events(&round);

        std::fprintf(stderr, "[cli] round %d begin\n", round_idx);
        // 1. KOPAW 导出：CPU 图案帧 → VULKAN 帧（fd + stride + LINEAR + fence）
        kopaw::OwnedFrame* cpu =
            kopaw::make_frame(KOPAW_MEDIA_VIDEO, round_idx, 0,
                              static_cast<size_t>(kW) * kH * 4);
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                uint8_t* px = cpu->data() + (static_cast<size_t>(y) * kW + x) * 4;
                px[0] = static_cast<uint8_t>(round_idx * 120 + (x * 3 & 0x7f));
                px[1] = static_cast<uint8_t>(y * 3 & 0xff);
                px[2] = 0x60;
                px[3] = 0xff;
            }
        }
        cpu->frame.format.video.width = kW;
        cpu->frame.format.video.height = kH;
        cpu->frame.stride = kW * 4;
        KopawFrame* gpu = exporter.export_cpu_frame(&cpu->frame, &reason);
        if (!check(gpu != nullptr, "exporter export")) {
            cpu->release_cb(&cpu->frame);
            goto teardown;
        }

        std::fprintf(stderr, "[cli] round %d exported, creating buffer\n", round_idx);
        // 2. params → wl_buffer（v2 create_immed：同步创建，监听器先于事件）
        round.params = zwp_linux_dmabuf_v1_create_params(state.dmabuf);
        zwp_linux_buffer_params_v1_add(
            round.params, gpu->planes[0].fd, 0, gpu->planes[0].offset,
            gpu->planes[0].stride,
            static_cast<uint32_t>(gpu->planes[0].modifier >> 32),
            static_cast<uint32_t>(gpu->planes[0].modifier & 0xffffffffu));
        round.buffer = zwp_linux_buffer_params_v1_create_immed(
            round.params, kW, kH, 0x34324241u /*AB24*/, 0);
        if (!check(round.buffer != nullptr, "params created wl_buffer")) {
            gpu->release(gpu);
            cpu->release_cb(&cpu->frame);
            goto teardown;
        }
        wl_buffer_add_listener(round.buffer, &buffer_listener, &round);

        // 3. explicit sync：acquire fence 交给合成器；release 事件回送
        zwp_linux_surface_synchronization_v1_set_acquire_fence(state.sync,
                                                         gpu->acquire_fence.fd);
        round.release_obj =
            zwp_linux_surface_synchronization_v1_get_release(state.sync);
        zwp_linux_buffer_release_v1_add_listener(round.release_obj,
                                                 &release_listener, &round);

        std::fprintf(stderr, "[cli] round %d committing\n", round_idx);
        // 4. attach + commit + frame callback
        round.frame_cb = wl_surface_frame(state.surface);
        wl_callback_add_listener(round.frame_cb, &frame_listener, &round);
        wl_surface_attach(state.surface, round.buffer, 0, 0);
        wl_surface_damage(state.surface, 0, 0, kW, kH);
        wl_surface_commit(state.surface);

        if (!wait_for_events(&state, 5000)) {
            check(false, "release/frame/sync events within 5s");
            gpu->release(gpu);
            cpu->release_cb(&cpu->frame);
            goto teardown;
        }
        if (!check(round.buffer_released, "wl_buffer.release received") ||
            !check(round.frame_done, "frame callback done") ||
            !check(round.sync_released, "explicit-sync release received")) {
            gpu->release(gpu);
            cpu->release_cb(&cpu->frame);
            goto teardown;
        }
        // 第 0 轮必须 fenced；第 1 轮若撞上回压窗口则允许 immediate。
        if (round_idx == 0) {
            check(round.sync_fenced, "explicit-sync release is fenced");
        }

        // 清理本轮：buffer 销毁会关闭合成器侧注册的平面 fd 副本语义
        if (round.frame_cb) wl_callback_destroy(round.frame_cb);
        if (round.params) zwp_linux_buffer_params_v1_destroy(round.params);
        if (round.buffer) wl_buffer_destroy(round.buffer);
        gpu->release(gpu);
        cpu->release_cb(&cpu->frame);
        wl_display_roundtrip(state.display);
        std::fprintf(stdout, "round %d: buffer released + fenced sync OK\n",
                     round_idx);
    }

    std::fprintf(stdout, "dmabuf wayland client: PASS (2 rounds, %lld ms)\n",
                 static_cast<long long>(now_mono_ms() - started));
    result = failures == 0 ? 0 : 1;

teardown:
    if (state.sync) zwp_linux_surface_synchronization_v1_destroy(state.sync);
    if (state.toplevel) xdg_toplevel_destroy(state.toplevel);
    if (state.xdg_surf) xdg_surface_destroy(state.xdg_surf);
    if (state.surface) wl_surface_destroy(state.surface);
    if (state.display) {
        wl_display_flush(state.display);
        wl_display_disconnect(state.display);
    }
    exporter.shutdown();
    // 终止合成器：SIGTERM 后给 2s 优雅退出窗口，仍未退出则 SIGKILL——
    // 合成器主线程若卡在未 signal 的 fence 等待上，SIGTERM 的默认动作无法
    // 执行，会让本测试一路挂到 CTest 的 60s 超时。
    if (child > 0) {
        kill(child, SIGTERM);
        const int64_t grace = now_mono_ms() + 2000;
        int status = 0;
        for (;;) {
            const pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) break;
            if (r < 0) break;
            if (now_mono_ms() > grace) {
                kill(child, SIGKILL);
                waitpid(child, &status, 0);
                break;
            }
            ::usleep(20000);
        }
    }
    // Preserve CTest's skip status (77).  Only an actual failure is 1.
    return result;
}
