// KOPMS 嵌套输出（M1）：GLFW + GL 3.3 窗口呈现最新提交的 SHM surface。
// M2 的 DRM/KMS 直出将替换本模块（同一 present() 接口）。
// 输入：GLFW 真实输入（光标/按键）经 InputCallbacks 交给合成器输入管线；
// 内容矩形用于指针坐标 → surface 坐标的 1:1 映射。
#pragma once
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdint>
#include <functional>
#include <string>

namespace kopms {

class NestedOutput {
public:
    // GLFW 原始输入回调（合成器转成 wl_pointer/wl_keyboard 事件）
    struct InputCallbacks {
        std::function<void(double, double)> cursor;              // 窗口坐标
        std::function<void(int, int)> mouse_button;              // button, action
        std::function<void(int, int, int, int)> key;             // key, scancode, action, mods
    };
    // 当前内容矩形（窗口坐标；无内容时 w/h 为 0）
    struct ContentRect {
        int x = 0, y = 0, w = 0, h = 0;
    };

    bool init(int width, int height, const std::string& title);
    // 呈现一帧 SHM 缓冲（当前支持 ARGB8888/XRGB8888）
    void present(const void* data, int32_t w, int32_t h, int32_t stride, uint32_t shm_fmt);
    // 绘制 + 交换（垂直同步节拍）
    void render_frame();
    void shutdown();
    bool should_close() const;

    void set_input_callbacks(InputCallbacks callbacks);
    ContentRect content_rect() const;

private:
    bool ensure_texture(int32_t w, int32_t h, uint32_t fmt);
    GLFWwindow* win_ = nullptr;
    unsigned int program_ = 0, vao_ = 0, tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    uint32_t fmt_ = 0;
    bool inited_ = false;
    InputCallbacks callbacks_{};
};

}  // namespace kopms
