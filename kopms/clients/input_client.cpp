// KOPMS 输入反馈客户端（P3 输入/输出持久测试）：
//   - 绑定 wl_seat 指针/键盘，接收点击与键入事件；
//   - GUI 反馈：点击位置画白色方块标记、键入字符用 3x5 位图字体渲染、
//     计数器实时刷新——每次事件触发重绘 + commit（视觉回路闭合）；
//   - stdout 输出 CLICK/KEY/MOTION 计数行（供 kopms-input-soak-test 核对）。
// 用法: WAYLAND_DISPLAY=kop-0 kopms-input-client [秒数=30]
#include <cerrno>
#include <poll.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "wayland-client.h"
#include "xdg-shell-client-protocol.h"

#include "keymap_table.hpp"  // evdev_to_char（与合成器共用同一张表）

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;

// 3x5 位图字体（'0'-'9' 'a'-'z' '-' ':'），每字 15 位：5 行 × 3 列，高位在前。
uint16_t glyph_of(char c) {
    static const uint16_t kDigits[10] = {
        0b111101101101111, 0b010110010010111, 0b111001111100111, 0b111001111001111,
        0b101101111001001, 0b111100111001111, 0b111100111101111, 0b111001001010010,
        0b111101111101111, 0b111101111001111,
    };
    static const uint16_t kLower[26] = {
        0b000111101101111, 0b100100111101111, 0b000111100100111, 0b001001111101111,
        0b000111111100111, 0b011100111100100, 0b111100111001111, 0b100100111101101,
        0b010000010010111, 0b011001001101010, 0b100101110101101, 0b100100100100111,
        0b000101111111101, 0b000110101101101, 0b000111101101111, 0b000111101111100,
        0b000101101111001, 0b000101110100100, 0b000111100011111, 0b010010111010010,
        0b000101101101111, 0b000101101101010, 0b000101101111111, 0b000101010101101,
        0b000101101111001, 0b000111010100111,
    };
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    if (c >= 'a' && c <= 'z') return kLower[c - 'a'];
    if (c == '-') return 0b000000111000000;
    if (c == ':') return 0b000010000010000;
    return 0;
}

struct PixelBuffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    void* data = nullptr;
    size_t size = 0;
    bool released = true;
};

struct Click {
    int x = 0;
    int y = 0;
};

struct Client {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    uint32_t seat_version = 0;
    xdg_wm_base* wm_base = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdg_sfc = nullptr;
    xdg_toplevel* top = nullptr;

    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_surface* pointer_focus = nullptr;
    int32_t cur_x = -1;
    int32_t cur_y = -1;
    bool shift = false;

    PixelBuffer buffers[2];
    int front = 0;

    // 反馈状态
    std::vector<Click> clicks;
    std::string typed;
    uint64_t click_count = 0;
    uint64_t key_count = 0;
    uint64_t motion_count = 0;
    int configured = 0;
    bool running = true;
};

// ---------------------------------------------------------------------------

void draw_rect(uint32_t* px, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; ++j) {
        for (int i = x; i < x + w; ++i) {
            if (i >= 0 && i < kW && j >= 0 && j < kH) px[j * kW + i] = color;
        }
    }
}

void draw_text(uint32_t* px, const std::string& text, int x, int y, int scale,
               uint32_t color) {
    int cx = x;
    for (char raw : text) {
        const uint16_t glyph = glyph_of(raw);
        if (glyph != 0 || raw == 'a') {
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if ((glyph >> (14 - row * 3 - col)) & 1) {
                        draw_rect(px, cx + col * scale, y + row * scale, scale, scale,
                                  color);
                    }
                }
            }
        }
        cx += 4 * scale;
    }
}

void render(Client* c, PixelBuffer* buf) {
    uint32_t* px = buf->pixels;
    // 背景（点击闪烁：最近一次点击后短暂变亮）
    const uint32_t bg = (c->click_count % 2) ? 0xff30251a : 0xff2a1f14;
    for (int i = 0; i < kW * kH; ++i) px[i] = bg;
    // 点击标记（白方块 + 边框）
    for (const Click& click : c->clicks) {
        draw_rect(px, click.x - 8, click.y - 8, 16, 16, 0xffffffff);
        draw_rect(px, click.x - 5, click.y - 5, 10, 10, 0xff3060ff);
    }
    // 键入文本 + 计数器
    draw_text(px, "input", 16, 16, 4, 0xff40a0ff);
    draw_text(px, c->typed, 16, 48, 6, 0xffffffff);
    draw_text(px, std::to_string(c->click_count), 16, kH - 72, 8, 0xffffff40);
    draw_text(px, ":", 120, kH - 72, 8, 0xffffff40);
    draw_text(px, std::to_string(c->key_count), 168, kH - 72, 8, 0xffffff40);
}

void commit(Client* c) {
    PixelBuffer& buf = c->buffers[c->front];
    if (!buf.released) return;  // 上一帧未被释放（合成器呈现为同步路径，不应发生）
    render(c, &buf);
    wl_surface_attach(c->surface, buf.buffer, 0, 0);
    wl_surface_damage(c->surface, 0, 0, kW, kH);
    wl_surface_commit(c->surface);
    buf.released = false;
    c->front ^= 1;
}

bool make_buffer(Client* c, PixelBuffer* out) {
    const size_t size = static_cast<size_t>(kW) * kH * 4;
    const int memfd = memfd_create("kopms-input-client", MFD_CLOEXEC);
    if (memfd < 0) return false;
    if (ftruncate(memfd, static_cast<off_t>(size)) != 0) {
        ::close(memfd);
        return false;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (data == MAP_FAILED) {
        ::close(memfd);
        return false;
    }
    wl_shm_pool* pool = wl_shm_create_pool(c->shm, memfd, static_cast<int32_t>(size));
    out->buffer = wl_shm_pool_create_buffer(
        pool, 0, kW, kH, kW * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(memfd);
    out->pixels = static_cast<uint32_t*>(data);
    out->data = data;
    out->size = size;
    out->released = true;
    return true;
}

// ---------------------------------------------------------------------------
// 指针/键盘事件
// ---------------------------------------------------------------------------

void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface* surface,
                   wl_fixed_t sx, wl_fixed_t sy) {
    auto* c = static_cast<Client*>(data);
    c->pointer_focus = surface;
    c->cur_x = wl_fixed_to_int(sx);
    c->cur_y = wl_fixed_to_int(sy);
}

void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto* c = static_cast<Client*>(data);
    c->pointer_focus = nullptr;
}

void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto* c = static_cast<Client*>(data);
    c->cur_x = wl_fixed_to_int(sx);
    c->cur_y = wl_fixed_to_int(sy);
    ++c->motion_count;
    std::printf("MOTION %llu\n", static_cast<unsigned long long>(c->motion_count));
}

void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                    uint32_t state) {
    auto* c = static_cast<Client*>(data);
    if (button != 0x110 || state != 1) return;  // 仅左键按下
    ++c->click_count;
    c->clicks.push_back({c->cur_x, c->cur_y});
    if (c->clicks.size() > 64) c->clicks.erase(c->clicks.begin());
    std::printf("CLICK %llu %d %d\n",
                static_cast<unsigned long long>(c->click_count), c->cur_x, c->cur_y);
    commit(c);  // GUI 反馈：点击标记上屏
}

void pointer_frame(void*, wl_pointer*) {}

const wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = nullptr,
    .frame = pointer_frame,
    .axis_source = nullptr,
    .axis_stop = nullptr,
    .axis_discrete = nullptr,
};

void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd,
                     uint32_t size) {
    (void)data;
    (void)format;
    (void)size;
    ::close(fd);  // 客户端自行解析（soak 用字符表核对）
}

void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    (void)data;
}

void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) { (void)data; }

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key,
                  uint32_t state) {
    auto* c = static_cast<Client*>(data);
    if (state != 1) return;
    const char ch = kopms::evdev_to_char(key, c->shift);
    if (ch == 0) return;
    ++c->key_count;
    c->typed.push_back(ch);
    if (c->typed.size() > 24) c->typed.erase(0, c->typed.size() - 24);
    std::printf("KEY %llu %c\n", static_cast<unsigned long long>(c->key_count), ch);
    commit(c);  // GUI 反馈：键入字符上屏
}

void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                        uint32_t, uint32_t, uint32_t) {
    auto* c = static_cast<Client*>(data);
    c->shift = (depressed & 1) != 0;
}

void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// ---------------------------------------------------------------------------

void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* c = static_cast<Client*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && !c->pointer) {
        c->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(c->pointer, &pointer_listener, c);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && !c->keyboard) {
        c->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(c->keyboard, &keyboard_listener, c);
    }
}

void seat_name(void*, wl_seat*, const char* name) { (void)name; }

const wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

void buffer_release(void* data, wl_buffer* buffer) {
    auto* c = static_cast<Client*>(data);
    for (PixelBuffer& buf : c->buffers) {
        if (buf.buffer == buffer) buf.released = true;
    }
}

const wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

void handle_wm_ping(void* data, xdg_wm_base* base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener wm_base_listener = {.ping = handle_wm_ping};

void handle_xdg_configure(void* data, xdg_surface*, uint32_t serial) {
    auto* c = static_cast<Client*>(data);
    xdg_surface_ack_configure(c->xdg_sfc, serial);
    if (!c->configured) {
        c->configured = 1;
        commit(c);  // 首帧
    }
}

const xdg_surface_listener s_xdg_surface_listener = {.configure = handle_xdg_configure};

void registry_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                     uint32_t version) {
    auto* c = static_cast<Client*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        c->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        c->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        c->seat_version = version > 5 ? 5 : version;
        c->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, c->seat_version));
        wl_seat_add_listener(c->seat, &seat_listener, c);
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

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    int seconds = 30;
    if (argc > 1) seconds = std::atoi(argv[1]);

    Client c;
    c.display = wl_display_connect(nullptr);
    if (!c.display) {
        std::fprintf(stderr, "input-client: cannot connect to compositor\n");
        return 2;
    }
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    wl_display_roundtrip(c.display);
    if (!c.compositor || !c.shm || !c.seat || !c.wm_base) {
        std::fprintf(stderr, "input-client: globals incomplete\n");
        return 2;
    }

    c.surface = wl_compositor_create_surface(c.compositor);
    c.xdg_sfc = xdg_wm_base_get_xdg_surface(c.wm_base, c.surface);
    xdg_surface_add_listener(c.xdg_sfc, &s_xdg_surface_listener, &c);
    c.top = xdg_surface_get_toplevel(c.xdg_sfc);
    xdg_toplevel_set_title(c.top, "kopms input feedback");
    if (!make_buffer(&c, &c.buffers[0]) || !make_buffer(&c, &c.buffers[1])) {
        std::fprintf(stderr, "input-client: shm buffer creation failed\n");
        return 2;
    }
    wl_surface_commit(c.surface);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    const int display_fd = wl_display_get_fd(c.display);
    while (c.running) {
        if (wl_display_dispatch_pending(c.display) > 0) continue;
        if (wl_display_flush(c.display) < 0 && errno != EAGAIN) break;
        pollfd pfd{display_fd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, 50);
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && wl_display_dispatch(c.display) < 0) break;
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    std::printf("DONE clicks=%llu keys=%llu motions=%llu\n",
                static_cast<unsigned long long>(c.click_count),
                static_cast<unsigned long long>(c.key_count),
                static_cast<unsigned long long>(c.motion_count));
    wl_display_disconnect(c.display);
    return 0;
}
