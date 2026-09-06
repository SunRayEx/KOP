#include "input_state.hpp"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <list>
#include <vector>

#include "kop/log.h"
#include "keymap_table.hpp"
#include "wayland-server.h"
#include "wayland-server-protocol.h"

#if __has_include(<xkbcommon/xkbcommon.h>)
#include <xkbcommon/xkbcommon.h>
#define KOPMS_HAVE_XKBCOMMON 1
#endif

namespace kopms {

static const char* kTag = "kopms-input";

namespace {

constexpr int kGlfwPress = 1;
constexpr int kGlfwRelease = 0;
constexpr int kGlfwRepeat = 2;
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;
constexpr uint32_t kEvdevPress = 1;
constexpr uint32_t kEvdevRelease = 0;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void pointer_send_frame_if_v5(wl_resource* pointer) {
    if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
        wl_pointer_send_frame(pointer);
    }
}

}  // namespace

struct InputState::Impl {
    struct SeatObject {
        Impl* self = nullptr;
        wl_resource* resource = nullptr;
        bool keyboard = false;
        wl_listener destroy{};
    };

    wl_display* display = nullptr;
    wl_event_loop* loop = nullptr;
    InputHooks hooks{};

    std::list<SeatObject> pointers;   // list：节点地址稳定（内嵌 destroy 监听）
    std::list<SeatObject> keyboards;

    wl_resource* pointer_focus_surface = nullptr;
    wl_resource* active_pointer = nullptr;
    wl_resource* keyboard_focus_surface = nullptr;
    wl_resource* active_keyboard = nullptr;
    int32_t last_sx = 0;
    int32_t last_sy = 0;
    bool pointer_inside = false;
    uint32_t shift_count = 0;
    std::vector<uint32_t> pressed_keys;

    int keymap_fd = -1;
    uint32_t keymap_size = 0;

    // soak 自驱动状态机：每 tick 执行一步
    static void seat_resource_destroyed(wl_listener* listener, void* data);
    wl_event_source* soak_timer = nullptr;
    uint32_t soak_rounds_left = 0;
    uint32_t soak_interval_ms = 0;
    uint32_t soak_step = 0;
    uint32_t soak_round = 0;
    // soak 键入文本
    std::string soak_text;
    size_t soak_text_pos = 0;

    SoakStats* stats = nullptr;

    uint32_t next_serial() { return wl_display_next_serial(display); }
    uint32_t now_stamp() { return static_cast<uint32_t>(now_ms() % 1000000); }

    void remove_resource(wl_resource* resource) {
        auto drop = [resource](std::list<SeatObject>& list) {
            for (auto it = list.begin(); it != list.end(); ++it) {
                if (it->resource == resource) {
                    list.erase(it);
                    return;
                }
            }
        };
        drop(pointers);
        drop(keyboards);
        if (active_pointer == resource) active_pointer = nullptr;
        if (active_keyboard == resource) active_keyboard = nullptr;
    }

    void pointer_leave_current() {
        if (!pointer_inside) return;
        if (active_pointer && pointer_focus_surface) {
            wl_pointer_send_leave(active_pointer, next_serial(),
                                  pointer_focus_surface);
            pointer_send_frame_if_v5(active_pointer);
        }
        pointer_inside = false;
        pointer_focus_surface = nullptr;
        active_pointer = nullptr;
    }

    void keyboard_leave_current() {
        if (!keyboard_focus_surface) return;
        if (active_keyboard && keyboard_focus_surface) {
            wl_keyboard_send_leave(active_keyboard, next_serial(),
                                   keyboard_focus_surface);
            pressed_keys.clear();
        }
        keyboard_focus_surface = nullptr;
        active_keyboard = nullptr;
    }

    template <typename Fn>
    void for_client_objects(wl_client* client, bool keyboard, Fn fn) {
        auto scan = [&](std::list<SeatObject>& list) {
            for (SeatObject& obj : list) {
                if (obj.keyboard == keyboard &&
                    wl_resource_get_client(obj.resource) == client) {
                    fn(obj);
                }
            }
        };
        if (keyboard) {
            scan(keyboards);
        } else {
            scan(pointers);
        }
    }

    bool keymap_ready() const { return keymap_fd >= 0 && keymap_size > 0; }

    void send_keymap(SeatObject& kb) {
        if (!keymap_ready()) return;
        wl_keyboard_send_keymap(kb.resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                keymap_fd, keymap_size);
        if (wl_resource_get_version(kb.resource) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
            wl_keyboard_send_repeat_info(kb.resource, 25, 600);
        }
    }

    void build_keymap() {
#ifdef KOPMS_HAVE_XKBCOMMON
        xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!ctx) return;
        xkb_rule_names names{};
        names.rules = "evdev";
        names.layout = "us";
        names.model = "pc105";
        xkb_keymap* map =
            xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!map) {
            xkb_context_unref(ctx);
            return;
        }
        const char* text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
        const size_t size = text ? strlen(text) : 0;
        if (size > 0) {
            keymap_fd = memfd_create("kopms-keymap", MFD_CLOEXEC);
            if (keymap_fd >= 0) {
                const ssize_t written = write(keymap_fd, text, size);
                if (written < 0 || static_cast<size_t>(written) != size) {
                    ::close(keymap_fd);
                    keymap_fd = -1;
                } else {
                    keymap_size = static_cast<uint32_t>(size);
                }
            }
        }
        xkb_keymap_unref(map);
        xkb_context_unref(ctx);
        if (keymap_ready()) {
            KOP_LOG_INFO(kTag, "键盘 keymap 已生成（%u 字节，evdev/us）", keymap_size);
        } else {
            KOP_LOG_WARN(kTag, "keymap 生成失败：键盘事件仍投递，客户端按无键Map处理");
        }
#else
        KOP_LOG_WARN(kTag, "libxkbcommon 不可用：跳过 keymap 生成");
#endif
    }

    // ------------------------------------------------------------------
    // soak 自驱动：每 tick 一步（move/click/key），多 tick 组成一轮
    // ------------------------------------------------------------------
    static int soak_tick(void* data) {
        auto* self = static_cast<Impl*>(data);
        if (!self || self->soak_rounds_left == 0) return 0;
        const int next_ms = soak_step_impl(self);
        if (next_ms > 0 && self->soak_timer) {
            wl_event_source_timer_update(self->soak_timer, next_ms);
        }
        return 0;
    }

    static int soak_step_impl(Impl* self) {
        if (self->hooks.input_ready && !self->hooks.input_ready()) {
            return 500;  // 客户端未就绪：等待重试
        }
        // 内容矩形（HiDPI/窗口缩放无关：与 cursor 回调同坐标系）
        InputRect rect;
        if (self->hooks.content_rect) rect = self->hooks.content_rect();
        if (rect.w < 120 || rect.h < 120) return 500;
        const int mx = rect.x + rect.w / 12;
        const int my = rect.y + rect.h / 12;
        const int span_x = rect.w - 2 * (rect.w / 12);
        const int span_y = rect.h - 2 * (rect.h / 12);
        constexpr uint32_t kMoves = 8;
        constexpr uint32_t kClicks = 5;

        if (self->soak_step < kMoves) {
            // 扫掠移动：对角折线路径
            const uint32_t i = self->soak_step;
            const int x = mx + span_x * static_cast<int>(i) / (kMoves - 1);
            const int y = my + (i % 2) * span_y;
            self->owner->cursor_pos(x, y);
        } else if (self->soak_step < kMoves + kClicks) {
            // 点击：网格位置（先移动到位再按下/释放）
            const uint32_t i = self->soak_step - kMoves;
            const int x = rect.x + rect.w / 6 +
                          static_cast<int>(i) * span_x / (kClicks - 1 + 1);
            const int y = rect.y + rect.h / 4 + (i % 2) * (rect.h / 3);
            self->owner->cursor_pos(x, y);
            self->owner->mouse_button(0, kGlfwPress);
            self->owner->mouse_button(0, kGlfwRelease);
        } else if (self->soak_text_pos < self->soak_text.size()) {
            // 键入：逐字符 press+release（大写先挂 shift）
            const char c = self->soak_text[self->soak_text_pos];
            const bool upper = c >= 'A' && c <= 'Z';
            const bool lower = c >= 'a' && c <= 'z';
            const bool digit = c >= '0' && c <= '9';
            const bool dash = c == '-';
            if (upper || lower || digit || dash) {
                const char lowered = upper ? static_cast<char>(c - 'A' + 'a') : c;
                // GLFW 字母键码 = 大写 ASCII（'A'=65），不是小写 ASCII！
                const int glfw_key = lowered == '-'
                                         ? 45
                                         : (lowered >= 'a' ? 'A' + (lowered - 'a')
                                                           : '0' + (lowered - '0'));
                if (upper) self->owner->keyboard_key(340, 0, kGlfwPress, 0x0001);
                self->owner->keyboard_key(glfw_key, 0, kGlfwPress, upper ? 0x0001 : 0);
                self->owner->keyboard_key(glfw_key, 0, kGlfwRelease, upper ? 0x0001 : 0);
                if (upper) self->owner->keyboard_key(340, 0, kGlfwRelease, 0);
            }
            ++self->soak_text_pos;
        } else {
            // 本轮结束
            ++self->stats->rounds_done;
            KOP_LOG_INFO(kTag, "soak 第 %u 轮完成（累计 motion=%llu click=%llu key=%llu）",
                         self->soak_round,
                         static_cast<unsigned long long>(self->stats->motions),
                         static_cast<unsigned long long>(self->stats->clicks),
                         static_cast<unsigned long long>(self->stats->keys));
            --self->soak_rounds_left;
            ++self->soak_round;
            self->soak_step = 0;
            self->soak_text_pos = 0;
            self->soak_text = "kopms-input-" + std::to_string(self->soak_round);
            if (self->soak_rounds_left > 0) {
                return self->soak_interval_ms;
            }
            KOP_LOG_INFO(kTag, "soak 全部轮次完成");
            self->soak_timer = nullptr;
            return 0;  // 停止定时器
        }
        ++self->soak_step;
        return 20;  // 步进间隔
    }

    InputState* owner = nullptr;
};

InputState::~InputState() { shutdown(); }

bool InputState::init(wl_display* display, wl_event_loop* loop, InputHooks hooks) {
    if (!display || !loop) return false;
    impl_ = new Impl();
    impl_->owner = this;
    impl_->display = display;
    impl_->loop = loop;
    impl_->hooks = std::move(hooks);
    impl_->stats = &soak_stats_;
    impl_->build_keymap();
    return true;
}

void InputState::shutdown() {
    if (!impl_) return;
    if (impl_->soak_timer) {
        wl_event_source_remove(impl_->soak_timer);
        impl_->soak_timer = nullptr;
    }
    if (impl_->keymap_fd >= 0) ::close(impl_->keymap_fd);
    delete impl_;
    impl_ = nullptr;
}

void InputState::Impl::seat_resource_destroyed(wl_listener* listener, void* data) {
    (void)data;
    auto* node = reinterpret_cast<SeatObject*>(
        reinterpret_cast<char*>(listener) - offsetof(SeatObject, destroy));
    if (node && node->self) {
        node->self->remove_resource(node->resource);
    }
    node->resource = nullptr;
}

void InputState::register_pointer(wl_resource* pointer) {
    if (!impl_ || !pointer) return;
    impl_->pointers.push_back({impl_, pointer, false, {}});
    Impl::SeatObject& node = impl_->pointers.back();
    node.destroy.notify = &InputState::Impl::seat_resource_destroyed;
    wl_list_init(&node.destroy.link);
    wl_resource_add_destroy_listener(pointer, &node.destroy);
}

void InputState::register_keyboard(wl_resource* keyboard) {
    if (!impl_ || !keyboard) return;
    impl_->keyboards.push_back({impl_, keyboard, true, {}});
    Impl::SeatObject& node = impl_->keyboards.back();
    node.destroy.notify = &InputState::Impl::seat_resource_destroyed;
    wl_list_init(&node.destroy.link);
    wl_resource_add_destroy_listener(keyboard, &node.destroy);
    impl_->send_keymap(node);
}

// ---------------------------------------------------------------------------
// 指针事件
// ---------------------------------------------------------------------------

void InputState::cursor_pos(double win_x, double win_y) {
    if (!impl_ || !impl_->hooks.pointer_target) return;
    Impl& s = *impl_;
    wl_resource* surface = nullptr;
    int32_t sx = 0;
    int32_t sy = 0;
    const bool hit = impl_->hooks.pointer_target(
        static_cast<int>(win_x), static_cast<int>(win_y), &surface, &sx, &sy);
    if (!hit || !surface) {
        s.pointer_leave_current();
        return;
    }
    const bool focus_changed = surface != s.pointer_focus_surface;
    s.last_sx = sx;
    s.last_sy = sy;
    if (focus_changed || !s.pointer_inside) {
        s.pointer_leave_current();
        wl_client* client = wl_resource_get_client(surface);
        s.pointer_focus_surface = surface;
        s.for_client_objects(client, false, [&](Impl::SeatObject& obj) {
            if (s.active_pointer == nullptr) s.active_pointer = obj.resource;
        });
        if (!s.active_pointer) {
            // 客户端尚未创建指针对象：等下一次 motion 重试
            s.pointer_focus_surface = nullptr;
            return;
        }
        wl_pointer_send_enter(s.active_pointer, s.next_serial(), surface, sx, sy);
        pointer_send_frame_if_v5(s.active_pointer);
        s.pointer_inside = true;
        ++soak_stats_.pointer_enters;
        KOP_LOG_DEBUG(kTag, "pointer enter surface=%p (%d,%d)",
                      static_cast<void*>(surface), sx, sy);
        return;
    }
    if (s.active_pointer) {
        wl_pointer_send_motion(s.active_pointer, s.now_stamp(), sx, sy);
        pointer_send_frame_if_v5(s.active_pointer);
        ++soak_stats_.motions;
    }
}

void InputState::mouse_button(int glfw_button, int action) {
    if (!impl_) return;
    Impl& s = *impl_;
    // 点击建立键盘焦点（文本输入前置条件）。
    if (action == kGlfwPress && s.pointer_focus_surface &&
        s.keyboard_focus_surface != s.pointer_focus_surface) {
        wl_resource* surface = s.pointer_focus_surface;
        wl_client* client = wl_resource_get_client(surface);
        s.keyboard_leave_current();
        s.keyboard_focus_surface = surface;
        s.for_client_objects(client, true, [&](Impl::SeatObject& obj) {
            if (s.active_keyboard == nullptr) s.active_keyboard = obj.resource;
        });
        if (s.active_keyboard) {
            wl_keyboard_send_modifiers(s.active_keyboard, s.next_serial(),
                                       s.shift_count, 0, 0, 0);
            wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(s.active_keyboard, s.next_serial(), surface,
                                   &keys);
            wl_array_release(&keys);
            ++soak_stats_.keyboard_enters;
            KOP_LOG_DEBUG(kTag, "keyboard enter surface=%p",
                          static_cast<void*>(surface));
        } else {
            s.keyboard_focus_surface = nullptr;
        }
    }
    if (!s.active_pointer || !s.pointer_inside) return;
    uint32_t button = 0;
    if (glfw_button == 0) button = kBtnLeft;
    else if (glfw_button == 1) button = kBtnRight;
    else if (glfw_button == 2) button = kBtnMiddle;
    if (button == 0) return;
    const uint32_t state = action == kGlfwPress ? kEvdevPress : kEvdevRelease;
    wl_pointer_send_button(s.active_pointer, s.next_serial(), s.now_stamp(), button,
                           state);
    pointer_send_frame_if_v5(s.active_pointer);
    if (action == kGlfwPress) ++soak_stats_.clicks;
}

// ---------------------------------------------------------------------------
// 键盘事件
// ---------------------------------------------------------------------------

void InputState::keyboard_key(int glfw_key, int, int action, int mods) {
    (void)mods;
    if (!impl_) return;
    Impl& s = *impl_;
    if (!s.active_keyboard || !s.keyboard_focus_surface) return;
    if (action == kGlfwRepeat) return;  // 重复键由客户端 repeat_info 处理
    const uint32_t evdev = glfw_key_to_evdev(glfw_key);
    if (evdev == 0) return;
    const bool is_shift = evdev == kEvdevLeftShift || evdev == kEvdevRightShift;
    if (is_shift) {
        if (action == kGlfwPress) ++s.shift_count;
        if (action == kGlfwRelease && s.shift_count > 0) --s.shift_count;
        wl_keyboard_send_modifiers(s.active_keyboard, s.next_serial(),
                                   s.shift_count, 0, 0, 0);
        return;
    }
    const uint32_t state = action == kGlfwPress ? kEvdevPress : kEvdevRelease;
    if (action == kGlfwPress) {
        s.pressed_keys.push_back(evdev);
    } else {
        for (auto it = s.pressed_keys.begin(); it != s.pressed_keys.end(); ++it) {
            if (*it == evdev) {
                s.pressed_keys.erase(it);
                break;
            }
        }
    }
    wl_keyboard_send_key(s.active_keyboard, s.next_serial(), s.now_stamp(), evdev,
                         state);
    if (action == kGlfwPress) ++soak_stats_.keys;
}

// ---------------------------------------------------------------------------
// soak 自驱动
// ---------------------------------------------------------------------------

void InputState::start_soak(uint32_t rounds, uint32_t interval_ms) {
    if (!impl_ || rounds == 0) return;
    if (impl_->soak_timer) {
        wl_event_source_remove(impl_->soak_timer);
        impl_->soak_timer = nullptr;
    }
    impl_->soak_rounds_left = rounds;
    impl_->soak_interval_ms = interval_ms ? interval_ms : 100;
    impl_->soak_step = 0;
    impl_->soak_round = 1;
    impl_->soak_text = "kopms-input-1";
    impl_->soak_text_pos = 0;
    KOP_LOG_INFO(kTag, "soak 自驱动启动：%u 轮，间隔 %ums", rounds,
                 impl_->soak_interval_ms);
    // 第一 tick 立即执行，随后按步进间隔推进
    impl_->soak_timer = wl_event_loop_add_timer(
        impl_->loop, &InputState::Impl::soak_tick, impl_);
    if (impl_->soak_timer) wl_event_source_timer_update(impl_->soak_timer, 1);
}

}  // namespace kopms
