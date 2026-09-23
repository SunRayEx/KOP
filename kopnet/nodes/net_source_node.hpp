// KOPAW 网络源节点：把 KOPNET 隧道的一条通道变成图中的源端口。
//
// 自驱动节点（self_driven=1）：run() 里 dial 端点、打开通道、循环收包，
// 每条消息按帧信封（frame_envelope）重建成一帧——CPU 帧带载荷字节，
// DMA-BUF 帧带平面 fd 与 acquire fence fd（仅 Unix 系传输）——再 emit
// 到图输出端口。远程 KOPMS 的输入事件流、远端解码后的媒体包都经此进入
// 本地图。回压：隧道的 credit + 通道收队列一直传到远端的发送队列，
// 远端图随之减速。
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "kopaw_abi.h"
#include "kopnet/tunnel.hpp"  // ChannelMode

struct KopawGraph;

namespace kopaw {

class NetChannel;

class NetSourceNode {
public:
    struct Options {
        std::string endpoint;  // tcp:// / unix: / ssh:// / relay://
        std::string kind = "media";
        kopnet::ChannelMode mode = kopnet::ChannelMode::Stream;
        uint32_t idle_timeout_ms = 15000;  // 收包空闲上限（0 = 仅响应 stop）
        uint32_t queue_capacity = 64;      // 出边队列容量
        // 断线自动重连：开启后传输断开时自动重拨，重拨成功后通道自动重开，
        // 本节点的逻辑通道号保持不变。fallback_endpoints 为故障转移列表
        // （主端点拨不通时按序尝试；每次重连都从主端点开始）
        bool auto_reconnect = false;
        std::vector<std::string> fallback_endpoints;
        uint32_t reconnect_base_delay_ms = 500;
        uint32_t reconnect_max_delay_ms = 5000;
    };

    explicit NetSourceNode(Options options);
    ~NetSourceNode();
    NetSourceNode(const NetSourceNode&) = delete;
    NetSourceNode& operator=(const NetSourceNode&) = delete;

    // 建立隧道并打开通道（run 前调用一次）。失败时 error 给出原因。
    bool open(std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    // 注册后由应用回填输出句柄（kopaw_graph_node_output）
    void set_output(KopawOutput out) { out_ = out; }

    // 引擎线程入口（vtable.run 转发）
    int32_t run_impl();
    // vtable.stop 转发
    void request_stop() { stopped_.store(true, std::memory_order_release); }

    uint64_t frames_emitted() const { return frames_emitted_; }

private:
    Options options_;
    KopawGraph* graph_ = nullptr;
    KopawOutput out_{};
    std::unique_ptr<NetChannel> channel_;
    std::atomic<bool> stopped_{false};
    uint64_t frames_emitted_ = 0;
};

}  // namespace kopaw
