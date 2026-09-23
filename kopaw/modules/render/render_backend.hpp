// 渲染后端抽象：Vulkan / OpenGL 编译期可选（工厂 + 接口）。
// 约定：GLFW 窗口由宿主在主线程创建；init/draw/poll/shutdown 全部
// 在渲染节点线程上调用（Linux 下 GLFW 允许单线程独占使用）。
#pragma once
#include <algorithm>
#include <string>

#include "kopaw_abi.h"  // 自带 extern "C" 保护
#include "native_dmabuf.hpp"

struct GLFWwindow;

namespace kopaw {

// letterbox 计算：视频保持纵横比缩放并居中于窗口
struct LetterboxRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

inline LetterboxRect compute_letterbox(uint32_t video_w, uint32_t video_h, uint32_t fb_w,
                                       uint32_t fb_h) {
    LetterboxRect r{};
    if (!video_w || !video_h || !fb_w || !fb_h) {
        r.w = static_cast<int32_t>(fb_w);
        r.h = static_cast<int32_t>(fb_h);
        return r;
    }
    const double scale = std::min(static_cast<double>(fb_w) / video_w,
                                  static_cast<double>(fb_h) / video_h);
    r.w = static_cast<int32_t>(video_w * scale);
    r.h = static_cast<int32_t>(video_h * scale);
    r.x = (static_cast<int32_t>(fb_w) - r.w) / 2;
    r.y = (static_cast<int32_t>(fb_h) - r.h) / 2;
    return r;
}

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // 初始化图形上下文（Vulkan 实例/设备/交换链等）
    virtual bool init(GLFWwindow* window, std::string* error) = 0;
    // 上传一帧 RGBA 并呈现；返回 false 表示应终止（设备丢失等）
    virtual bool draw(const KopawFrame* frame) = 0;
    // 轮询窗口事件（GLFW）
    virtual void poll_events() = 0;
    // 用户是否关闭了窗口
    virtual bool window_closed() const = 0;
    // 释放资源（窗口销毁由宿主负责）
    virtual void shutdown() = 0;

    virtual const char* name() const = 0;

    // 是否支持直接消费外部内存帧（KOPAW_MEMORY_DMABUF）。
    // The mask is negotiated before native output is enabled. Per-frame
    // modifier compatibility remains an import-time check and can trigger the
    // CPU fallback.
    virtual uint32_t dmabuf_format_mask() const {
        return kNativeDmabufFormatNone;
    }
    virtual bool supports_dmabuf() const { return false; }
};

// 工厂：name 为 "vulkan"/"opengl"。
// 返回 nullptr 且 *error 被填充表示该后端不可用（未编译/初始化失败）。
IRenderBackend* create_render_backend(const std::string& name, std::string* error);

}  // namespace kopaw
