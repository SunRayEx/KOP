// P3-M3/M4：KOPMS Vulkan GPU 合成场景。
//
// 职责边界：
//   - 导入：BUS2LAYER（KOPAW 帧）或 linux-dmabuf（Wayland wl_buffer）交付的
//     DMA-BUF 平面在 Vulkan 侧物化为 VkImage（vkImportMemoryFdKHR），
//     acquire fence 经有界 poll 等待（explicit-sync 消费点）；
//   - 合成：多窗口 GPU 合成——布局由调用方按窗口树/焦点/ownership 计算后
//     传入，场景只负责绘制与 z 序；
//   - 生命周期：窗口回压（每窗口同时在飞一帧，超出即拒收 DROPPED），
//     present/render fence 完成后才回调释放帧，DRM 直出模式由 page-flip
//     完成事件驱动释放；
//   - 与 M1 的 CPU wl_shm 路径无关，禁止互相复用。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kopaw_abi.h"

struct GLFWwindow;

namespace kopms {

class VulkanScene {
public:
    struct Rect {
        int32_t x = 0, y = 0;
        uint32_t w = 0, h = 0;
    };

    // 布局项：window_id 唯一标识场景窗口（CONTROL 窗口用其 id，Wayland
    // surface 用合成器生成的 id）；z 越大越后绘制（焦点窗口最后）。
    struct LayoutItem {
        uint64_t window_id = 0;
        uint64_t z = 0;
        Rect rect;
        bool focused = false;
    };

    // 导入请求：平面/fence fd 由调用方持有（SCM_RIGHTS 物化的本地 fd），
    // 本类只在导入期间借用，不关闭。
    struct ImportRequest {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;  // DRM fourcc（XR24/AR24 等）
        uint32_t plane_count = 0;
        const int* plane_fds = nullptr;
        const uint32_t* plane_offsets = nullptr;
        const uint32_t* plane_strides = nullptr;
        const uint64_t* plane_modifiers = nullptr;
        uint32_t acquire_fence_kind = KOPAW_SYNC_FENCE_NONE;
        int acquire_fence_fd = -1;
        uint64_t session_id = 0;   // 释放回调参数
        uint32_t frame_id = 0;     // 释放回调参数
    };

    // present/render fence 完成后逐帧回调（DRM 直出模式由 flip 驱动）。
    // window_id 用于合成器区分 BUS 帧（按 session/frame_id 走 BUS release）
    // 与 Wayland dmabuf buffer（发送 wl_buffer.release）。
    using ReleaseFn =
        std::function<void(uint64_t window_id, uint64_t session_id, uint32_t frame_id)>;
    using DroppedFn = std::function<void(uint64_t session_id, uint32_t frame_id)>;

    struct Options {
        bool windowed = false;  // GLFW Vulkan 窗口呈现；否则离屏（fence 节拍）
        uint32_t width = 1280;
        uint32_t height = 720;
        int fence_timeout_ms = 100;  // acquire fence 有界等待
    };

    VulkanScene();
    ~VulkanScene();
    VulkanScene(const VulkanScene&) = delete;
    VulkanScene& operator=(const VulkanScene&) = delete;

    bool init(const Options& options, GLFWwindow* window, std::string* error);
    void shutdown();
    bool inited() const { return inited_; }

    void set_release_callback(ReleaseFn release) { release_fn_ = std::move(release); }
    void set_dropped_callback(DroppedFn dropped) { dropped_fn_ = std::move(dropped); }

    // 提交一帧到场景窗口。false = 该窗口上一帧仍在飞（回压拒收），调用方
    // 应以 DROPPED 释放。导入失败同样返回 false（帧被拒收而不是挂死）。
    bool submit(uint64_t window_id, const ImportRequest& request, std::string* error);

    // 合成一帧布局并呈现。windowed 模式走 swapchain present，离屏模式渲染
    // 后等待 render fence；两种模式都在 GPU 完成后触发帧释放回调。
    bool render(const std::vector<LayoutItem>& layout, std::string* error);
    // 取最近一次已完成合成的 release fence（dup 出新 fd），无则 -1。供
    // Wayland explicit-sync 的 fenced_release 使用。
    int take_release_fence();

    // DRM 直出模式：render() 不等待 fence 也不释放帧；export_composite_dmabuf
    // 等待合成 fence 后导出，page-flip 完成后由调用方触发
    // complete_direct_present() 释放（M3：flip 完成前不释放帧）。
    void set_direct_mode(bool enabled) { direct_mode_ = enabled; }
    bool direct_mode() const { return direct_mode_; }

    // DRM 直出：把当前合成结果导出为线性 DMA-BUF（由调用方 AddFB2 +
    // page_flip；flip 完成前不得释放场景窗口持有的帧）。
    bool export_composite_dmabuf(int* fd, uint32_t* stride, uint32_t* width,
                                 uint32_t* height, std::string* error);
    // flip 完成后调用：释放上一组合成窗口的帧。
    void complete_direct_present();

    struct Stats {
        uint64_t imported_frames = 0;
        uint64_t presented_frames = 0;
        uint64_t dropped_frames = 0;
        uint64_t rejected_frames = 0;  // 回压拒收
        uint64_t import_us_total = 0;
        uint64_t fence_wait_us_total = 0;
        uint64_t composite_us_total = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool inited_ = false;
    bool direct_mode_ = false;
    ReleaseFn release_fn_;
    DroppedFn dropped_fn_;
};

}  // namespace kopms
