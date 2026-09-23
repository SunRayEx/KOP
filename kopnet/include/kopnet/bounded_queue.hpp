// KOPNET：有界阻塞队列（每通道收/发队列）。
//
// 沿用 KOPAW 的“有界队列 = 天然背压”哲学：队列满时生产者阻塞（有界超时），
// 压力沿 Tunnel 传到对端 credit，最终传到 KOPAW 图的链路队列。
// 通道关闭后：put 返回 Closed 并丢弃载荷，get 返回 Closed 并清空队列。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kopnet {

struct Packet {
    std::vector<uint8_t> data;
    std::vector<int> fds;  // Unix 传输的 SCM_RIGHTS 附属 fd（get 方负责关闭）
};

enum class QueueStatus {
    Ok,
    Closed,
    Timeout,
};

class BoundedPacketQueue {
public:
    explicit BoundedPacketQueue(size_t capacity) : capacity_(capacity) {}

    // 阻塞入队（timeout_ms<0 表示无限等待）。返回 Closed 表示队列已关闭。
    // 所有权约定：仅 Ok 时才接管 Packet（含其 fd）；Timeout/Closed 时调用方保留
    // 所有权，可重试或自行回收 fd——重试循环里重复传同一个包是安全的。
    QueueStatus put(Packet&& pkt, int timeout_ms);
    // 非阻塞入队：满时返回 Timeout（不阻塞，供背压探测）。
    QueueStatus try_put(Packet&& pkt);
    // 阻塞出队。
    QueueStatus get(Packet* pkt, int timeout_ms);
    // 非阻塞出队：空时返回 Timeout。
    QueueStatus try_get(Packet* pkt);
    // 当前长度（近似，诊断用）
    size_t size_approx();
    void close();
    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::vector<Packet> queue_;
    bool closed_ = false;
};

}  // namespace kopnet
