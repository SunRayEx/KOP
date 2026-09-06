// KOP 公共库：单调时间工具
#pragma once
#include <cstdint>

namespace kop {

// 进程启动以来的单调微秒数
int64_t steady_us();

// 挂起当前线程；对极短间隔退化为自旋，保证毫秒级精度
void sleep_us(int64_t us);

}  // namespace kop
