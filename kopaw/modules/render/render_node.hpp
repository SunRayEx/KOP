// 渲染节点（自驱动 sink）：拉取 RGBA 帧 → 按媒体时钟节拍 → 后端绘制呈现。
#pragma once
#include <atomic>
#include <functional>
#include <string>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "kopaw_abi.h"
#include "render/render_backend.hpp"

namespace kopaw {

class RenderNode {
public:
    RenderNode(KopawGraph* g, GLFWwindow* window, IRenderBackend* backend)
        : g_(g), win_(window), backend_(backend) {}

    ~RenderNode() { delete backend_; }

    KopawNodeDesc desc();
    void set_node_id(uint32_t id) { node_id_ = id; }
    void set_dmabuf_fallback(std::function<void()> callback) {
        dmabuf_fallback_ = std::move(callback);
    }
    void set_dmabuf_capability_callback(std::function<void(uint32_t)> callback) {
        dmabuf_capability_callback_ = std::move(callback);
    }

    // 窗口被用户关闭（player 轮询此标记以主动停止图）
    std::atomic<bool>& close_requested() { return close_requested_; }

    // 引擎线程入口（vtable.run 转发至此）
    int32_t run_impl();

private:
    KopawGraph* g_;
    GLFWwindow* win_;
    IRenderBackend* backend_;
    uint32_t node_id_ = 0;
    std::atomic<bool> close_requested_{false};
    std::function<void()> dmabuf_fallback_;
    std::function<void(uint32_t)> dmabuf_capability_callback_;
    bool dmabuf_fallback_used_ = false;
};

}  // namespace kopaw
