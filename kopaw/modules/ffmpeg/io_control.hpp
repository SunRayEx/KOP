// FFmpeg 阻塞 I/O 的停止与超时控制。
//
// AVIOInterruptCB 只提供“是否中断”一个回调，因此把停止、当前操作
// deadline 和超时分类状态集中在这个小对象里。对象必须比 FFmpeg 的
// AVFormatContext 更长寿，且 callback 不得访问已析构的调用方数据。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace kopaw {

struct FfmpegIoControl {
    std::atomic<bool>* stop = nullptr;
    std::atomic<int64_t> deadline_us{0};
    std::atomic<bool> timed_out{false};

    static int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void begin(uint32_t timeout_ms) {
        begin_until(timeout_ms == 0
                        ? 0
                        : now_us() + static_cast<int64_t>(timeout_ms) * 1000);
    }

    void begin_until(int64_t deadline) {
        timed_out.store(false, std::memory_order_release);
        deadline_us.store(deadline, std::memory_order_release);
    }

    void end() { deadline_us.store(0, std::memory_order_release); }

    bool was_timed_out() const { return timed_out.load(std::memory_order_acquire); }
};

inline int ffmpeg_interrupt_callback(void* opaque) {
    auto* control = static_cast<FfmpegIoControl*>(opaque);
    if (!control) return 0;
    if (control->stop && control->stop->load(std::memory_order_acquire)) return 1;
    const int64_t deadline = control->deadline_us.load(std::memory_order_acquire);
    if (deadline != 0 && FfmpegIoControl::now_us() >= deadline) {
        control->timed_out.store(true, std::memory_order_release);
        return 1;
    }
    return 0;
}

}  // namespace kopaw
