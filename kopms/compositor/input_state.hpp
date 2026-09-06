// P3 输入事件管线：把 nested 输出的真实 GLFW 输入（鼠标点击/移动、键盘）
// 与 soak 自驱动脚本翻译成 wl_pointer/wl_keyboard 事件，投递给聚焦的客户端
// surface。
//
// 聚焦策略（M1 兼容路径）：指针落点由合成器提供的 pointer_target 回调解析
// （最近提交内容的 xdg 主 surface 的 1:1 内容矩形）；键盘焦点跟随指针点击。
// 事件顺序遵循 wl_pointer/wl_keyboard 协议：enter→motion→button→frame，
// 键盘 keymap→enter→modifiers→key；v5+ 指针在每批事件后发送 frame。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct wl_display;
struct wl_event_loop;
struct wl_resource;

namespace kopms {

struct InputRect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct InputHooks {
    // 解析输出坐标下的指针目标：命中返回 true 并给出 surface 资源与
    // surface 内坐标；未命中（空白处）返回 false。
    std::function<bool(int win_x, int win_y, wl_resource** surface, int32_t* sx,
                       int32_t* sy)>
        pointer_target;
    // 是否已有可聚焦的客户端 surface（soak 门控：无客户端时不注入事件）。
    std::function<bool()> input_ready;
    // 当前内容矩形（与 cursor 回调同一坐标系）；soak 在其中取点击/移动点。
    std::function<InputRect()> content_rect;
};

class InputState {
public:
    InputState() = default;
    ~InputState();
    InputState(const InputState&) = delete;
    InputState& operator=(const InputState&) = delete;

    // display 用于 serial；loop 用于 soak 定时器；hooks 提供指针目标解析。
    bool init(wl_display* display, wl_event_loop* loop, InputHooks hooks);
    void shutdown();

    // seat_get_pointer/keyboard 时登记客户端资源（析构由 destroy 监听兜底）。
    void register_pointer(wl_resource* pointer);
    void register_keyboard(wl_resource* keyboard);

    // NestedOutput 真实输入回调（GLFW 语义）。
    void cursor_pos(double win_x, double win_y);
    void mouse_button(int glfw_button, int action);
    void keyboard_key(int glfw_key, int scancode, int action, int mods);

    // 持久压测自驱动：rounds 轮（移动扫掠 + 5 次点击 + 键入
    // "kopms-input-<round>"），每轮间隔 interval_ms；rounds=0 关闭。
    void start_soak(uint32_t rounds, uint32_t interval_ms);

    struct SoakStats {
        uint64_t motions = 0;
        uint64_t clicks = 0;
        uint64_t keys = 0;
        uint64_t pointer_enters = 0;
        uint64_t keyboard_enters = 0;
        uint64_t rounds_done = 0;
    };
    const SoakStats& soak_stats() const { return soak_stats_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    SoakStats soak_stats_{};
};

// GLFW key → Linux evdev 键码；不可映射返回 0。客户端（kopms-input-client）
// 复用同一张表做键入字符回读。
uint32_t glfw_key_to_evdev(int glfw_key);

// evdev 键码 + shift → 字符（仅覆盖 soak 键入的字符集）；0 = 不可打印。
char evdev_to_char(uint32_t evdev, bool shift);

}  // namespace kopms
