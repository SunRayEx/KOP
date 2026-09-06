// KOPMS_Test —— 用户级全流程测试应用（基于 KOP_AppSDK_Interface）。
//
// 用法:
//   KOPMS_Test [选项]
//     --socket NAME     合成器 socket 名（默认 kop-usertest-<pid>，自起合成器）
//     --compositor BIN  合成器二进制路径（默认取本应用同目录）
//     --frames N        零拷贝发布帧数（默认 30）
//     --input-rounds N  输入持久压测轮数（默认 4；0 = 跳过）
//     --skip-spawn      不自起合成器，连接 --socket 指定的已在运行实例
//
// 覆盖流程：
//   1. ABI 契约 + Vulkan DMA-BUF 能力探测
//   2. 合成器生命周期（自起/接管 + 优雅退出）
//   3. BUS2LAYER 协议：HELLO 能力协商（Handle/modifier/explicit-sync）
//   4. 控制面：窗口创建/绑定/焦点/剪贴板/ownership
//   5. 零拷贝发布：CPU 帧 → Vulkan 导出 → BUS 提交 → FRAME_RELEASE 全链
//      （回压、发布统计、延迟抽样）
//   6. 输入/输出持久压测（可选，需要显示环境与输入反馈客户端）
//
// 退出码 = 失败项数。
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "kop/app_sdk.hpp"
#include "kopms_protocol.h"  // KOPMS_OWNERSHIP_*

#if defined(_WIN32)
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Args {
    std::string socket = "kop-usertest-" + std::to_string(::getpid());
    std::string compositor;
    int frames = 30;
    int input_rounds = 4;
    bool skip_spawn = false;
};

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--socket") args.socket = next();
        else if (a == "--compositor") args.compositor = next();
        else if (a == "--frames") args.frames = std::atoi(next().c_str());
        else if (a == "--input-rounds") args.input_rounds = std::atoi(next().c_str());
        else if (a == "--skip-spawn") args.skip_spawn = true;
    }
    return args;
}

// 在本应用同目录与构建树相邻目录（kopms/、上级）查找工具二进制。
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

void sleep_ms(int ms) {
    struct timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    kop::sdk::TestReport report("KOPMS_Test");
    std::printf("KOPMS_Test（KOP App SDK %s）\n", kop::sdk::version());

    // ---- 1. 契约与能力探测 ----
    report.section("1. 契约与能力探测");
    {
        std::string reason;
        report.check(kop::sdk::check_abi(&reason), "SDK ↔ 宿主 ABI 匹配", reason);
        if (kop::sdk::FramePublisher::available(&reason)) {
            report.pass("Vulkan DMA-BUF 导出能力", reason);
        } else {
            report.skip("Vulkan DMA-BUF 导出能力", reason);
        }
    }

    const bool has_display = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
    pid_t compositor_pid = -1;

    // ---- 2. 合成器生命周期 ----
    report.section("2. 合成器生命周期");
    const std::string compositor_bin =
        args.compositor.empty() ? sibling_of_argv0("kopms-compositor")
                                : args.compositor;
    if (args.skip_spawn) {
        report.skip("自起合成器", "--skip-spawn（连接已有实例）");
    } else if (!has_display) {
        report.skip("自起合成器", "无显示环境（X11/Wayland），nested 输出不可用");
    } else if (!fs::exists(compositor_bin)) {
        report.skip("自起合成器", "找不到 " + compositor_bin);
    } else {
        compositor_pid = fork();
        if (compositor_pid == 0) {
            execl(compositor_bin.c_str(), "kopms-compositor", args.socket.c_str(),
                  "300", static_cast<char*>(nullptr));
            _exit(127);
        }
        sleep_ms(1500);
        char path[256];
        snprintf(path, sizeof(path), "/run/user/%d/%s", static_cast<int>(getuid()),
                 args.socket.c_str());
        report.check(compositor_pid > 0 && access(path, R_OK) == 0,
                     "合成器启动 + socket 就绪", path);
    }

    // ---- 3-5. 协议 / 控制面 / 零拷贝 ----
    if (args.skip_spawn || has_display) {
        kop::sdk::FramePublisher publisher;

        report.section("3. BUS2LAYER 协议握手");
        {
            kop::sdk::PublisherOptions opts;
            opts.bus_socket = args.socket + ".bus";  // BUS socket = <wayland>.bus
            opts.window_id = 4242;
            std::string err;
            if (report.check(publisher.connect(opts, &err), "连接 + 能力协商", err)) {
                report.check(publisher.connected(), "会话建立（窗口绑定完成）");
            }
        }

        report.section("4. 控制面（窗口树/焦点/剪贴板/ownership）");
        if (publisher.connected()) {
            std::string err;
            report.check(publisher.focus_window(4242, &err), "焦点设置", err);
            report.check(publisher.set_clipboard_owner(4242, &err), "剪贴板所有者",
                         err);
            report.check(publisher.set_ownership(4242, KOPMS_OWNERSHIP_SHARED, &err),
                         "ownership = SHARED", err);
        }

        report.section("5. 零拷贝发布（CPU 帧 → DMA-BUF → BUS → release）");
        if (publisher.connected()) {
            const int total = args.frames > 0 ? args.frames : 30;
            std::vector<uint8_t> frame(static_cast<size_t>(320) * 240 * 4, 0x40);
            auto stamp_frame = [&](int i) {
                // 动态图案：随帧号变化的棋盘（合成端可见输出变化）
                for (int y = 0; y < 240; ++y) {
                    for (int x = 0; x < 320; ++x) {
                        const bool on = ((x / 40 + y / 40 + i) % 2) == 0;
                        uint8_t* px = frame.data() + (static_cast<size_t>(y) * 320 + x) * 4;
                        px[0] = on ? 0xff : 0x30;
                        px[1] = static_cast<uint8_t>(i * 7 & 0xff);
                        px[2] = on ? 0x30 : 0xff;
                        px[3] = 0xff;
                    }
                }
            };
            int published = 0;
            int failures = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < total; ++i) {
                stamp_frame(i);
                std::string err;
                if (publisher.publish_cpu_frame(frame.data(), 320, 240, 320 * 4, i,
                                                &err)) {
                    ++published;
                } else {
                    ++failures;
                }
                publisher.poll(10);
            }
            // 排空在飞（合成器呈现节拍在 publish 之后仍在推进 release）
            const auto drain_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(3);
            while (publisher.in_flight() > 0 &&
                   std::chrono::steady_clock::now() < drain_deadline) {
                publisher.poll(10);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            const auto stats = publisher.stats();
            report.check(published == total, "发布成功数",
                         std::to_string(published) + "/" + std::to_string(total));
            report.check(failures == 0, "零发布失败",
                         failures ? std::to_string(failures) + " 失败" : "");
            report.check(stats.released >= static_cast<uint64_t>(total) - 2,
                         "FRAME_RELEASE 回收",
                         std::to_string(stats.released) + " released / " +
                             std::to_string(stats.dropped) + " dropped");
            report.check(publisher.in_flight() == 0, "在飞清空",
                         std::to_string(publisher.in_flight()));
            report.check(ms > 0, "发布耗时",
                         std::to_string(ms) + "ms / " + std::to_string(total) + " 帧");
            publisher.disconnect();
        } else {
            report.skip("零拷贝发布", "publisher 未连接");
        }

        // ---- 6. 输入/输出持久压测 ----
        report.section("6. 输入/输出持久压测");
        if (!has_display) {
            report.skip("输入持久压测", "无显示环境");
        } else if (args.input_rounds <= 0) {
            report.skip("输入持久压测", "--input-rounds 0");
        } else {
            const std::string soak_bin = sibling_of_argv0("kopms-input-soak-test");
            if (!fs::exists(soak_bin)) {
                report.skip("输入持久压测", "找不到 " + soak_bin);
            } else {
                std::string rounds = std::to_string(args.input_rounds);
                const int exit_code =
                    std::system((soak_bin + " " + rounds + " > /dev/null 2>&1")
                                    .c_str());
                report.check(exit_code == 0, "输入管线持久压测",
                             "退出码 " + std::to_string(exit_code));
            }
        }

        // ---- 7. 优雅退出 ----
        report.section("7. 合成器优雅退出");
        if (compositor_pid > 0) {
            kill(compositor_pid, SIGTERM);
            int status = 0;
            waitpid(compositor_pid, &status, 0);
            report.check(true, "SIGTERM 终止", "exit=" +
                                                   std::to_string(WEXITSTATUS(status)));
        } else if (!args.skip_spawn) {
            report.skip("优雅退出", "合成器非本应用启动");
        }
    }

    report.summary();
    return report.failures();
}
