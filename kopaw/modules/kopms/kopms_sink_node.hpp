// P3-M3：KOPAW → KOPMS 零拷贝汇聚节点。
//
// 数据面：引擎投递 CPU RGBA 帧 → VulkanDmabufExporter 上传为可导出
// VkImage（KOPAW_MEMORY_VULKAN）→ KopmsFrameDescriptor 包裝 → BUS2LAYER
// FRAME_SUBMIT（planes/acquire fence 经 SCM_RIGHTS 过 socket）。
// 生命周期：FRAME_RELEASE 到达时沿 descriptor.release → KopawFrame.release
// 逐层归还，最后一个引用把图像还给导出器空闲池并关闭 fd。
// 回压：在飞帧数达 max_in_flight 时 send 阻塞泵 FRAME_RELEASE，引擎队列
// 随之填满、上游减速——这是 M4 帧回压的生产端。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "kopaw_abi.h"
#include "render/vulkan/vk_dma_export.hpp"

struct KopawGraph;
struct KopmsFrameDescriptor;

namespace kopaw {

class KopmsSinkNode {
public:
    struct Options {
        std::string bus_socket;        // BUS socket 名（XDG_RUNTIME_DIR 相对）
        uint64_t window_id = 1;        // CONTROL 窗口 id（connect 时创建并 attach）
        bool manage_window = true;     // 发送 WINDOW_CREATE/WINDOW_ATTACH
        uint32_t max_in_flight = 3;    // 在飞帧上限（回压边界）
        uint32_t max_export_images = 8;
    };

    explicit KopmsSinkNode(Options options);
    ~KopmsSinkNode();
    KopmsSinkNode(const KopmsSinkNode&) = delete;
    KopmsSinkNode& operator=(const KopmsSinkNode&) = delete;

    // 连接 BUS 并完成能力协商 + 窗口绑定；失败时 error 返回原因。
    bool connect(std::string* error);
    void disconnect();

    KopawNodeDesc desc();
    void set_graph(KopawGraph* graph, uint32_t node_id) {
        graph_ = graph;
        node_id_ = node_id;
    }

    uint64_t frames_submitted() const { return frames_submitted_; }
    uint64_t frames_released() const { return frames_released_; }
    uint64_t frames_dropped() const { return frames_dropped_; }

private:
    struct Submission;
    struct Impl;

    // BUS descriptor 的 retain/release 桥（Submission 私有性需要成员函数）
    static void desc_retain(KopmsFrameDescriptor* desc);
    static void desc_release(KopmsFrameDescriptor* desc);

    int32_t send_impl(KopawFrame* frame);
    void drain(bool wait_all);
    void pump_releases(int timeout_ms);
    void free_submission(Submission* sub);
    bool setup_window(std::string* error);

    Options options_;
    std::unique_ptr<Impl> impl_;
    VulkanDmabufExporter exporter_;
    KopawGraph* graph_ = nullptr;
    uint32_t node_id_ = 0;
    bool connected_ = false;
    bool sink_done_ = false;
    // 在飞提交表：key = Submission 指针。client 对 descriptor 的 release
    // 会经 free_submission 自动移除对应条目。
    std::unordered_map<uintptr_t, Submission*> pending_;

    uint64_t frames_submitted_ = 0;
    uint64_t frames_released_ = 0;
    uint64_t frames_dropped_ = 0;
};

}  // namespace kopaw
