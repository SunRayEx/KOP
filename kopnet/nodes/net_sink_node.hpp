// KOPAW 网络汇聚节点：把图中到达的帧发到 KOPNET 隧道的一条通道。
//
// 响应式 sink（is_sink=1, inputs=1）：send() 收到帧后按帧信封
// （frame_envelope：定长头 + 平面记录 + 载荷）序列化，平面 fd 与
// acquire fence fd 随该消息透传，随后释放帧。
// fd 必须 dup：隧道的发送是异步的（worker 线程稍后才 sendmsg），而
// 帧的 release 会立刻关闭 fd——直接传 fd 会引发 fd 号复用错传。
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

class NetSinkNode {
public:
    struct Options {
        std::string endpoint;
        std::string kind = "media";
        kopnet::ChannelMode mode = kopnet::ChannelMode::Stream;
        uint32_t send_timeout_ms = 5000;
        uint32_t queue_capacity = 64;
        bool auto_reconnect = false;
        std::vector<std::string> fallback_endpoints;
        uint32_t reconnect_base_delay_ms = 500;
        uint32_t reconnect_max_delay_ms = 5000;
    };

    explicit NetSinkNode(Options options);
    ~NetSinkNode();
    NetSinkNode(const NetSinkNode&) = delete;
    NetSinkNode& operator=(const NetSinkNode&) = delete;

    // 建立隧道、打开通道。失败时 error 给出原因。
    bool open(std::string* error);

    KopawNodeDesc desc();
    void set_graph(KopawGraph* graph, uint32_t node_id) {
        graph_ = graph;
        node_id_ = node_id;
    }

    // vtable.send 转发（帧所有权已转移给本节点）
    int32_t send_impl(KopawFrame* frame);
    void request_stop() { stopped_.store(true, std::memory_order_release); }

    uint64_t frames_sent() const { return frames_sent_; }
    uint64_t frames_dropped() const { return frames_dropped_; }

private:
    Options options_;
    KopawGraph* graph_ = nullptr;
    uint32_t node_id_ = 0;
    std::unique_ptr<NetChannel> channel_;
    std::atomic<bool> stopped_{false};
    uint64_t frames_sent_ = 0;
    uint64_t frames_dropped_ = 0;
};

}  // namespace kopaw
