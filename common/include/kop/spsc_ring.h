// KOP 公共库：单生产者/单消费者无锁环形缓冲（float 采样）。
// 专用于音频汇聚节点的实时回调路径：生产者 = 节点 send 线程，
// 消费者 = PortAudio 回调线程。容量为 2 的幂。
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace kop {

class SpscRingF32 {
public:
    explicit SpscRingF32(size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(new float[capacity_pow2]) {}

    ~SpscRingF32() { delete[] buf_; }

    SpscRingF32(const SpscRingF32&) = delete;
    SpscRingF32& operator=(const SpscRingF32&) = delete;

    size_t capacity() const { return cap_; }

    // 可写字节数（生产者线程调用）
    size_t write_space() const {
        size_t r = head_cache_;  // 仅用于减少伪共享，允许过期
        size_t w = tail_.load(std::memory_order_relaxed);
        return cap_ - (w - r);
    }

    // 可读元素数（消费者线程调用）
    size_t read_space() const {
        size_t w = tail_cache_;
        size_t r = head_.load(std::memory_order_relaxed);
        return w - r;
    }

    // 生产者：写入 n 个元素，返回实际写入数（空间不足时部分写入）
    size_t write(const float* src, size_t n) {
        size_t w = tail_.load(std::memory_order_relaxed);
        size_t r = head_cache_;
        size_t space = cap_ - (w - r);
        if (n > space) n = space;
        size_t pos = w & mask_;
        size_t first = n < (cap_ - pos) ? n : (cap_ - pos);
        __builtin_memcpy(buf_ + pos, src, first * sizeof(float));
        __builtin_memcpy(buf_, src + first, (n - first) * sizeof(float));
        tail_.store(w + n, std::memory_order_release);
        head_cache_ = r;  // 保持
        return n;
    }

    // 消费者：读出 n 个元素，返回实际读出数
    size_t read(float* dst, size_t n) {
        size_t r = head_.load(std::memory_order_relaxed);
        size_t w = tail_cache_;
        size_t avail = w - r;
        if (n > avail) n = avail;
        size_t pos = r & mask_;
        size_t first = n < (cap_ - pos) ? n : (cap_ - pos);
        __builtin_memcpy(dst, buf_ + pos, first * sizeof(float));
        __builtin_memcpy(dst + first, buf_, (n - first) * sizeof(float));
        head_.store(r + n, std::memory_order_release);
        tail_cache_ = w;  // 保持
        return n;
    }

    // 生产者刷新消费者游标（send 路径写满前先同步一次，减少误判）
    void sync_consumer_pos() { head_cache_ = head_.load(std::memory_order_acquire); }
    // 消费者刷新生产者游标
    void sync_producer_pos() { tail_cache_ = tail_.load(std::memory_order_acquire); }

private:
    size_t cap_;
    size_t mask_;
    float* buf_;
    alignas(64) std::atomic<size_t> tail_{0};  // 生产者写
    alignas(64) std::atomic<size_t> head_{0};  // 消费者写
    alignas(64) size_t head_cache_ = 0;        // 生产者本地缓存
    alignas(64) size_t tail_cache_ = 0;        // 消费者本地缓存
};

}  // namespace kop
