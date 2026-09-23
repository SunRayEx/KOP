// KOPNET：有界阻塞队列实现。
#include "kopnet/bounded_queue.hpp"

namespace kopnet {

QueueStatus BoundedPacketQueue::put(Packet&& pkt, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (closed_) return QueueStatus::Closed;
    if (timeout_ms < 0) {
        not_full_.wait(lock, [this] { return queue_.size() < capacity_ || closed_; });
    } else if (timeout_ms > 0) {
        not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return queue_.size() < capacity_ || closed_; });
    } else if (queue_.size() >= capacity_) {
        return QueueStatus::Timeout;
    }
    if (closed_) return QueueStatus::Closed;
    if (queue_.size() >= capacity_) return QueueStatus::Timeout;
    queue_.push_back(std::move(pkt));
    not_empty_.notify_one();
    return QueueStatus::Ok;
}

QueueStatus BoundedPacketQueue::try_put(Packet&& pkt) {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) return QueueStatus::Closed;
    if (queue_.size() >= capacity_) return QueueStatus::Timeout;
    queue_.push_back(std::move(pkt));
    not_empty_.notify_one();
    return QueueStatus::Ok;
}

QueueStatus BoundedPacketQueue::get(Packet* pkt, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (timeout_ms < 0) {
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
    } else if (timeout_ms > 0) {
        not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [this] { return !queue_.empty() || closed_; });
    } else if (queue_.empty()) {
        return QueueStatus::Timeout;
    }
    if (queue_.empty()) {
        return closed_ ? QueueStatus::Closed : QueueStatus::Timeout;
    }
    *pkt = std::move(queue_.front());
    queue_.erase(queue_.begin());
    not_full_.notify_one();
    return QueueStatus::Ok;
}

QueueStatus BoundedPacketQueue::try_get(Packet* pkt) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) return closed_ ? QueueStatus::Closed : QueueStatus::Timeout;
    *pkt = std::move(queue_.front());
    queue_.erase(queue_.begin());
    not_full_.notify_one();
    return QueueStatus::Ok;
}

size_t BoundedPacketQueue::size_approx() {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

void BoundedPacketQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        // 关闭时丢弃滞留包（fd 由 Tunnel 层在关闭前统一回收，这里只清载荷）
        queue_.clear();
    }
    not_full_.notify_all();
    not_empty_.notify_all();
}

}  // namespace kopnet
