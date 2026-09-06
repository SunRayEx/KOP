// KOP_Demo —— 基于 KOP_AppSDK_Interface 的开发者演示应用。
//
// 演示内容（约 100 行用户代码，不接触任何内部节点/协议细节）：
//   1. kop::sdk::Pipeline       打开媒体 → 解码 → 帧回调（数据面）
//   2. 帧处理：在回调里给每帧叠加动态效果（移动色条 + 帧号）
//   3. kop::sdk::FramePublisher 把处理后的帧零拷贝发布到 KOPMS 合成器
//   4. 控制面：创建/绑定/聚焦演示窗口
//   5. 实时统计：FPS、发布/释放/在飞、周期性报告
//
// 用法:
//   KOP_Demo [选项]
//     --media PATH        媒体文件（默认 test_media.mkv，缺失自动生成）
//     --seconds N         最长演示时长（默认 10；媒体更短则自然结束）
//     --compositor BIN    合成器二进制（默认取本应用同目录构建树）
//     --socket NAME       合成器 socket 名（默认 kop-demo-<pid>）
//     --no-compositor     纯数据面模式：只跑管线并打印统计（无合成器也能跑）
//     --effect none|bar   帧处理效果（默认 bar 动态色条）
//
// 退出码：0 成功；非 0 = 关键步骤失败。
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "kop/app_sdk.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Args {
    std::string media = "test_media.mkv";
    int seconds = 10;
    std::string compositor;
    std::string socket = "kop-demo-" + std::to_string(static_cast<long>(getpid()));
    bool no_compositor = false;
    std::string effect = "bar";
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--media") args.media = next();
        else if (a == "--seconds") args.seconds = std::atoi(next().c_str());
        else if (a == "--compositor") args.compositor = next();
        else if (a == "--socket") args.socket = next();
        else if (a == "--no-compositor") args.no_compositor = true;
        else if (a == "--effect") args.effect = next();
    }
    return args;
}

std::string sibling_of_argv0(const char* name) {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return name;
    self[n] = '\0';
    std::string dir = self;
    dir = dir.substr(0, dir.find_last_of('/'));
    for (const std::string& base : {dir, dir + "/../kopms", dir + "/.."}) {
        const std::string candidate = base + "/" + name;
        if (fs::exists(candidate)) return candidate;
    }
    return name;
}

bool ensure_media(const std::string& media) {
    if (fs::exists(media)) return true;
    if (fs::exists("scripts/gen-test-media.sh")) {
        std::system(("scripts/gen-test-media.sh " + media + " 10 > /dev/null 2>&1")
                        .c_str());
    }
    return fs::exists(media);
}

void sleep_ms(int ms) {
    struct timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    std::printf("KOP_Demo（KOP App SDK %s）\n", kop::sdk::version());
    std::printf("  媒体: %s\n  模式: %s\n", args.media.c_str(),
                args.no_compositor ? "纯数据面（--no-compositor）"
                                   : "媒体 → 帧处理 → KOPMS 零拷贝合成");

    // ---- 0. 媒体准备 ----
    if (!ensure_media(args.media)) {
        std::fprintf(stderr, "demo: 媒体 %s 不存在且自动生成失败\n",
                     args.media.c_str());
        return 2;
    }

    // ---- 1. 连接 KOPMS 合成器（可选）----
    kop::sdk::FramePublisher publisher;
    pid_t compositor_pid = -1;
    if (!args.no_compositor) {
        std::string reason;
        if (!kop::sdk::FramePublisher::available(&reason)) {
            std::fprintf(stderr, "demo: Vulkan DMA-BUF 不可用（%s），退化为纯数据面\n",
                         reason.c_str());
            args.no_compositor = true;
        } else {
            const std::string compositor_bin =
                args.compositor.empty() ? sibling_of_argv0("kopms-compositor")
                                        : args.compositor;
            if (fs::exists(compositor_bin)) {
                compositor_pid = fork();
                if (compositor_pid == 0) {
                    execl(compositor_bin.c_str(), "kopms-compositor",
                          args.socket.c_str(), "300", static_cast<char*>(nullptr));
                    _exit(127);
                }
                sleep_ms(1500);
            }
            kop::sdk::PublisherOptions opts;
            opts.bus_socket = args.socket + ".bus";
            opts.window_id = 100;
            std::string err;
            if (!publisher.connect(opts, &err)) {
                std::fprintf(stderr, "demo: 连接合成器失败（%s）\n", err.c_str());
                if (compositor_pid > 0) {
                    kill(compositor_pid, SIGTERM);
                    waitpid(compositor_pid, nullptr, 0);
                }
                return 2;
            }
            publisher.focus_window(100, nullptr);
            std::printf("  已连接 KOPMS 合成器（bus=%s，窗口 100 已绑定并聚焦）\n",
                        opts.bus_socket.c_str());
        }
    }

    // ---- 2. 打开管线 + 帧处理 + 发布 ----
    const uint32_t kW = 320;
    const uint32_t kH = 240;
    std::vector<uint8_t> work(static_cast<size_t>(kW) * kH * 4, 0);

    uint64_t processed = 0;
    int64_t first_pts = -1;
    int64_t last_report = now_ms();
    const int64_t demo_start = now_ms();
    const int64_t demo_deadline = demo_start + static_cast<int64_t>(args.seconds) * 1000;

    kop::sdk::PipelineOptions opts;
    opts.media = args.media;
    opts.on_frame = [&](const kop::sdk::FrameView& f) {
        if (f.media_type != KOPAW_MEDIA_VIDEO || f.width == 0 || f.height == 0) return;
        // ---- 帧处理（演示：缩放到演示分辨率 + 动态效果 + 帧号标记）----
        // 真实应用在这里做算法处理（推理/叠加/转码……）；demo 用最近邻缩放。
        const double sx = static_cast<double>(f.width) / kW;
        const double sy = static_cast<double>(f.height) / kH;
        for (uint32_t y = 0; y < kH; ++y) {
            const uint32_t sy0 = static_cast<uint32_t>(y * sy);
            const uint8_t* src = f.data + static_cast<size_t>(sy0) * f.stride;
            uint8_t* dst = work.data() + static_cast<size_t>(y) * kW * 4;
            for (uint32_t x = 0; x < kW; ++x) {
                const uint8_t* s = src + static_cast<uint32_t>(x * sx) * 4;
                dst[x * 4 + 0] = s[0];
                dst[x * 4 + 1] = s[1];
                dst[x * 4 + 2] = s[2];
                dst[x * 4 + 3] = 0xff;
            }
        }
        if (args.effect == "bar") {
            // 动态色条：位置随帧号推进
            const int bar_x = static_cast<int>((processed * 7) % (kW - 40));
            for (int y = 0; y < static_cast<int>(kH); ++y) {
                for (int x = bar_x; x < bar_x + 40 && x < static_cast<int>(kW); ++x) {
                    uint8_t* px = work.data() + (static_cast<size_t>(y) * kW + x) * 4;
                    px[0] = 0x20;
                    px[1] = 0x80;
                    px[2] = 0xff;
                }
            }
        }
        ++processed;
        if (first_pts < 0) first_pts = f.pts;

        // ---- 零拷贝发布（SDK 一行完成 Vulkan 导出 + BUS 提交）----
        if (publisher.connected()) {
            std::string err;
            if (!publisher.publish_cpu_frame(work.data(), kW, kH, kW * 4, f.pts,
                                             &err)) {
                static int drop_logs = 0;
                if (++drop_logs <= 3) {
                    std::fprintf(stderr, "demo: 发布失败（%s）\n", err.c_str());
                }
            }
            publisher.poll(0);  // 泵 FRAME_RELEASE（回压回收）
        }

        // ---- 周期统计（GUI 反馈的数字化等价）----
        if (processed % 60 == 0) {
            const int64_t now = now_ms();
            const double fps = 60000.0 / std::max<int64_t>(1, now - last_report);
            last_report = now;
            const auto st = publisher.stats();
            std::printf("  [demo] 帧 %5llu | %6.1f fps | 发布 %llu / 释放 %llu / 在飞 %zu\n",
                        static_cast<unsigned long long>(processed), fps,
                        static_cast<unsigned long long>(st.submitted),
                        static_cast<unsigned long long>(st.released),
                        publisher.in_flight());
        }
    };

    std::string err;
    auto pipeline = kop::sdk::Pipeline::open(opts, &err);
    if (!pipeline) {
        std::fprintf(stderr, "demo: 管线打开失败（%s）\n", err.c_str());
        if (compositor_pid > 0) kill(compositor_pid, SIGTERM);
        return 2;
    }
    if (!pipeline->start(&err)) {
        std::fprintf(stderr, "demo: 管线启动失败（%s）\n", err.c_str());
        if (compositor_pid > 0) kill(compositor_pid, SIGTERM);
        return 2;
    }

    // ---- 3. 运行到时长上限或媒体自然结束 ----
    while (pipeline->state() == KOPAW_STATE_RUNNING && now_ms() < demo_deadline) {
        struct timespec ts{0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    pipeline->stop(2000);

    // ---- 4. 收尾报告 ----
    if (publisher.connected()) {
        const auto drain_deadline = now_ms() + 3000;
        while (publisher.in_flight() > 0 && now_ms() < drain_deadline) {
            publisher.poll(10);
        }
    }
    const auto st = publisher.stats();
    const double wall_s = (now_ms() - demo_start) / 1000.0;
    std::printf("\n== KOP_Demo 结果 ==\n");
    std::printf("  处理帧数   : %llu（%s）\n",
                static_cast<unsigned long long>(processed),
                pipeline->state() == KOPAW_STATE_FINISHED ? "媒体自然结束"
                                                          : "时长上限停止");
    std::printf("  处理帧率   : %.1f fps\n",
                wall_s > 0 ? processed / wall_s : 0.0);
    if (first_pts >= 0) {
        std::printf("  首帧 pts   : %lld us\n", static_cast<long long>(first_pts));
    }
    if (publisher.connected()) {
        std::printf("  零拷贝发布 : %llu 提交 / %llu 释放 / %llu 丢弃 / 在飞 %zu\n",
                    static_cast<unsigned long long>(st.submitted),
                    static_cast<unsigned long long>(st.released),
                    static_cast<unsigned long long>(st.dropped),
                    publisher.in_flight());
        std::printf("  合成器统计 : %s\n", pipeline->stats_json().c_str());
    }
    std::printf("  管线状态   : %d（%s）\n", pipeline->state(),
                pipeline->state() == KOPAW_STATE_FINISHED ? "FINISHED" : "STOPPING");

    publisher.disconnect();
    pipeline->stop(1000);
    if (compositor_pid > 0) {
        kill(compositor_pid, SIGTERM);
        waitpid(compositor_pid, nullptr, 0);
    }
    std::printf("KOP_Demo: 完成\n");
    return processed > 0 ? 0 : 1;
}
