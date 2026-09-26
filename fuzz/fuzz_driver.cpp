// KOP 模糊测试独立驱动实现。
#include "fuzz_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kop/log.h"

namespace kop_fuzz {
namespace {

// xorshift64*：固定起始种子，保证单次运行可复现；KOP_FUZZ_SEED 可覆盖。
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}

    uint64_t next() {
        uint64_t x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    size_t below(size_t n) {
        if (n == 0) return 0;
        return static_cast<size_t>(next() % n);
    }

    uint8_t byte() { return static_cast<uint8_t>(next() & 0xff); }

private:
    uint64_t state_ = 0;
};

uint64_t parse_seed_env() {
    if (const char* s = std::getenv("KOP_FUZZ_SEED")) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 0);
        if (end != s) return static_cast<uint64_t>(v);
    }
    return 0x123456789ABCDEFULL;
}

uint64_t parse_iters_env(uint64_t fallback) {
    if (const char* s = std::getenv("KOP_FUZZ_ITERS")) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 0);
        if (end != s) return static_cast<uint64_t>(v);
    }
    return fallback;
}

// 读文件为字节数组（文件模式：复现崩溃）。
bool read_file(const char* path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out->assign(static_cast<size_t>(n), 0);
    const size_t got =
        n == 0 ? 0 : std::fread(out->data(), 1, static_cast<size_t>(n), f);
    out->resize(got);
    std::fclose(f);
    return true;
}

// 单条变异：在 in 基础上产出一条新输入。覆盖常见报文畸变。
void mutate(Rng* rng, const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    out->assign(in.begin(), in.end());
    const size_t cap = 4096;
    switch (rng->below(8)) {
        case 0:  // 随机位置改一个字节
            if (!out->empty()) out->at(rng->below(out->size())) = rng->byte();
            break;
        case 1:  // 翻转一个比特位
            if (!out->empty()) {
                const size_t i = rng->below(out->size());
                out->at(i) ^= static_cast<uint8_t>(1u << rng->below(8));
            }
            break;
        case 2:  // 插入随机字节
            if (out->size() < cap) {
                const size_t at = rng->below(out->size() + 1);
                const size_t n = 1 + rng->below(8);
                out->insert(out->begin() + static_cast<long>(at), n, 0);
                for (size_t k = at; k < at + n; ++k) out->at(k) = rng->byte();
            }
            break;
        case 3:  // 截断
            if (!out->empty()) out->resize(rng->below(out->size()));
            break;
        case 4:  // 把某一段长度域写成“极大值”（解析器最易越界的地方）
            if (out->size() >= 4) {
                const size_t at = rng->below(out->size() - 3);
                for (int k = 0; k < 4; ++k)
                    out->at(at + k) = static_cast<uint8_t>((rng->next() >> (8 * k)) & 0xff);
            }
            break;
        case 5:  // 整段清零 / 全 0xff
            if (!out->empty()) {
                const uint8_t fill = rng->below(2) ? 0 : 0xff;
                std::fill(out->begin(), out->end(), fill);
            }
            break;
        case 6:  // 与另一条语料拼接
            // 由调用方在需要时处理；这里退化为复制
            break;
        default:  // 原样（让 libFuzzerTestOneInput 的“有效输入”路径被反复走）
            break;
    }
    if (out->size() > cap) out->resize(cap);
}

int g_crash_index = 0;

// 最近一次执行的输入：ASan 死亡回调据此落盘。
const std::vector<uint8_t>* g_last_input = nullptr;

// 发现崩溃时落盘，供文件模式复现。
void dump_crash(const std::vector<uint8_t>& buf) {
    char name[64];
    std::snprintf(name, sizeof(name), "crash-%d.bin", ++g_crash_index);
    FILE* f = std::fopen(name, "wb");
    if (f) {
        std::fwrite(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        KOP_LOG_ERROR("fuzz", "崩溃输入已写入 %s（长度 %zu）", name, buf.size());
    }
}

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define KOP_FUZZ_HAVE_ASAN 1
#  endif
#elif defined(__SANITIZE_ADDRESS__)
#  define KOP_FUZZ_HAVE_ASAN 1
#endif

#ifdef KOP_FUZZ_HAVE_ASAN
extern "C" void __sanitizer_set_death_callback(void (*)(void*));

void on_death(void*) {
    if (g_last_input) dump_crash(*g_last_input);
}
#endif

// 运行一次并统计；返回非 0 表示需停止。
int run_one(const std::vector<uint8_t>& buf, uint64_t* executed) {
    ++*executed;
    g_last_input = &buf;
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    g_last_input = nullptr;
    return 0;
}

}  // namespace

int run_standalone(int argc, char** argv) {
    Seeds seeds;
    kop_fuzz_make_seeds(&seeds);
    if (seeds.buffers.empty()) {
        // 无种子也得起步：给一条全零缓冲，变异会逐步改出各种结构。
        seeds.buffers.emplace_back(64, 0);
    }

    // ---- 文件模式：每个 argv 跑一次（复现 / 语料回归）----
    if (argc > 1) {
        int failures = 0;
        for (int i = 1; i < argc; ++i) {
            std::vector<uint8_t> buf;
            if (!read_file(argv[i], &buf)) {
                KOP_LOG_ERROR("fuzz", "无法读取语料 %s", argv[i]);
                ++failures;
                continue;
            }
            uint64_t executed = 0;
            run_one(buf, &executed);
            KOP_LOG_INFO("fuzz", "语料 %s（%zu 字节）执行完成", argv[i], buf.size());
        }
        return failures == 0 ? 0 : 1;
    }

    // ---- 变异模式 ----
    const uint64_t iters = parse_iters_env(20000);
#ifdef KOP_FUZZ_HAVE_ASAN
    __sanitizer_set_death_callback(&on_death);
#endif
    Rng rng(parse_seed_env());
    uint64_t executed = 0;
    std::vector<uint8_t> cur = seeds.buffers[rng.below(seeds.buffers.size())];
    std::vector<uint8_t> next;

    KOP_LOG_INFO("fuzz", "开始变异循环：%llu 次迭代，%zu 条种子（seed=0x%llx）",
                 static_cast<unsigned long long>(iters), seeds.buffers.size(),
                 static_cast<unsigned long long>(parse_seed_env()));

    for (uint64_t i = 0; i < iters; ++i) {
        // 每隔一段就回到某条种子，避免漂移到永远无效的区域
        if ((i & 0x3ff) == 0 || cur.empty()) {
            cur = seeds.buffers[rng.below(seeds.buffers.size())];
        }
        mutate(&rng, cur, &next);
        run_one(next, &executed);
        cur.swap(next);
    }

    KOP_LOG_INFO("fuzz", "模糊测试完成：%llu 次执行，无 ASan/UBSan 报错",
                 static_cast<unsigned long long>(executed));
    return 0;
}

}  // namespace kop_fuzz

// standalone 模式的 main。libFuzzer 模式（clang + -fsanitize=fuzzer）
// 下不参与编译，由 libFuzzer 提供 main。
#ifdef KOP_FUZZ_STANDALONE
int main(int argc, char** argv) {
    return kop_fuzz::run_standalone(argc, argv);
}
#endif
