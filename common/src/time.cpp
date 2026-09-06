#include "kop/time.h"

#include <chrono>
#include <thread>

namespace kop {

int64_t steady_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void sleep_us(int64_t us) {
    if (us <= 0) return;
    if (us >= 2000) {
        std::this_thread::sleep_for(std::chrono::microseconds(us - 1000));
        return;
    }
    // 短等待：忙等保证精度（MVP 视频节拍最多等一帧时长）
    auto deadline = steady_us() + us;
    while (steady_us() < deadline) {
        std::this_thread::yield();
    }
}

}  // namespace kop
