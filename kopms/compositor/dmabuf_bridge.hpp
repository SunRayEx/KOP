// P3-M4：Wayland 侧 native DMA-BUF 融合。
//
// 职责：
//   - zwp_linux_dmabuf_v1（v1-v3）：格式/modifier 协商、linux_buffer_params
//     → wl_buffer；wl_buffer 的一生持有平面 fd（SCM_RIGHTS 接收件）；
//   - wl_buffer → KOPMS Handle：buffer_view() 把 wl_buffer 转为
//     DmabufBufferView，由合成器提交进 Vulkan 场景（与 BUS 帧同一导入路径，
//     不经过 CPU wl_shm）；
//   - explicit synchronization（unstable v1）：set_acquire_fence 的 fd 在
//     commit 时取走交给场景导入；get_release 在帧呈现场景后按
//     fenced/immediate 语义回送。
#pragma once

#include <cstdint>
#include <string>

#include "kopaw_abi.h"

struct wl_display;
struct wl_resource;

namespace kopms {

// wl_buffer 中 DMA-BUF 内容的只读视图。fd 由 wl_buffer 资源持有，
// 消费方（场景导入）只借用，不关闭。
struct DmabufBufferView {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // DRM fourcc
    uint32_t plane_count = 0;
    int fds[KOPAW_MAX_DMABUF_PLANES] = {-1, -1, -1, -1};
    uint32_t offsets[KOPAW_MAX_DMABUF_PLANES] = {0, 0, 0, 0};
    uint32_t strides[KOPAW_MAX_DMABUF_PLANES] = {0, 0, 0, 0};
    uint64_t modifiers[KOPAW_MAX_DMABUF_PLANES] = {0, 0, 0, 0};
};

class DmabufBridge {
public:
    DmabufBridge() = default;
    ~DmabufBridge();
    DmabufBridge(const DmabufBridge&) = delete;
    DmabufBridge& operator=(const DmabufBridge&) = delete;

    // 创建 zwp_linux_dmabuf_v1 与 explicit-sync 全局（协议 XML 可用时）。
    bool install(wl_display* display, std::string* error);
    bool dmabuf_enabled() const { return dmabuf_enabled_; }
    bool explicit_sync_enabled() const { return explicit_sync_enabled_; }

    // ---- 合成器 commit 路径使用的静态工具 ----
    static bool is_dmabuf_buffer(wl_resource* buffer);
    // 提取 wl_buffer 内容；acquire_fence_fd 取走后返回（无则 -1），调用方
    // 用完必须 close。
    static bool buffer_view(wl_resource* buffer, DmabufBufferView* view,
                            int* acquire_fence_fd);
    // 场景呈现完成后调用：发送 wl_buffer.release。
    static void send_buffer_release(wl_resource* buffer);

    // ---- explicit-sync 状态（surface 键控）----
    // 同一 surface 复用一份状态；返回 void* 以免暴露内部类型，
    // 调用方按 SurfaceSync* 解释（仅限本桥实现文件）。
    void* sync_state_for(wl_resource* surface_resource);
    // 取走该 surface 当前 acquire fence（commit 消费点）。
    int take_acquire_fence(wl_resource* surface_resource);
    // 帧不再使用后回送 release 事件；release_fence_fd < 0 发 immediate。
    void fire_release(wl_resource* surface_resource, int release_fence_fd);
    // surface 销毁时清理（关闭 fence fd、取消 pending release）。
    void surface_destroyed(wl_resource* surface_resource);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool dmabuf_enabled_ = false;
    bool explicit_sync_enabled_ = false;
};

}  // namespace kopms
