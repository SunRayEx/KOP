// KOP 模糊测试：与 libFuzzer 约定兼容的独立驱动。
//
// 约定（与 LLVM libFuzzer 一致）：每个 harness 实现
//   extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len);
// 返回 0。本驱动在没有 libFuzzer 的环境下提供等价的运行能力：
//
//   1) 文件模式：把每个 argv 当作语料文件运行一次（复现崩溃用）。
//   2) 变异模式：从 harness 提供的种子语料出发做随机变异循环；发现
//      ASan/UBSan 报错时把触发输入写入 ./crash-<n> 便于复现。
//
// 同一份 harness 既能接入 libFuzzer（clang + -fsanitize=fuzzer），
// 也能在只有 GCC 的机器上跑（standalone + ASan/UBSan）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// harness 实现它；libFuzzer 模式下由 libFuzzer 直接调用。
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len);

namespace kop_fuzz {

// 种子语料：harness 提供若干结构上有效的缓冲，变异循环从这里起步。
// 纯随机字节几乎总是卡在“过短/魔数错误”的早返回，有种子才能深入解析器。
struct Seeds {
    std::vector<std::vector<uint8_t>> buffers;
};

// 由每个 harness 实现（全局符号：driver 与 harness 都用 C 链接约定查找）。
extern "C" void kop_fuzz_make_seeds(kop_fuzz::Seeds* out);

// 独立驱动入口。仅在 standalone 模式下被 harness 的 main() 调用。
int run_standalone(int argc, char** argv);

}  // namespace kop_fuzz
