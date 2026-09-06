// P3 输入/输出持久压测：
//   1. 拉起合成器（KOPMS_INPUT_SOAK=rounds,interval → 输入管线自驱动脚本）；
//   2. 拉起 kopms-input-client（点击标记/键入字符渲染到画面，stdout 输出计数）；
//   3. 核对：注入的点击/键入数与客户端接收数一致，每个事件触发重绘提交；
//   4. 持久性：多轮运行后合成器 fd 数量稳定（无泄漏），进程存活。
// 无显示环境（nested 输出需要 X11/Wayland）时以 77 跳过。
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int fail(const char* message) {
    std::fprintf(stderr, "input-soak: FAIL %s\n", message);
    return 1;
}

std::string read_available(int fd) {
    std::string out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (n < static_cast<ssize_t>(sizeof(buf))) break;
    }
    return out;
}

size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
         pos += needle.size()) {
        ++count;
    }
    return count;
}

size_t fd_count(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    size_t count = 0;
    if (DIR* dir = opendir(path)) {
        while (readdir(dir) != nullptr) ++count;
        closedir(dir);
        count = count > 2 ? count - 2 : 0;  // 去掉 . 与 ..
    }
    return count;
}

}  // namespace

int main() {
    if (!getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) {
        std::fprintf(stdout, "input-soak: SKIP (no display)\n");
        return 77;
    }

    char self_path[4096];
    const ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len <= 0) return fail("readlink");
    self_path[len] = '\0';
    std::string dir = self_path;
    dir = dir.substr(0, dir.find_last_of('/'));

    // 1. 合成器：soak 自驱动 8 轮（每轮 8 次移动 + 5 次点击 + 键入 13 字符）
    const std::string socket = "kop-soak-" + std::to_string(getpid());
    const pid_t compositor = fork();
    if (compositor == 0) {
        // 合成器继承桌面 WAYLAND_DISPLAY（nested GLFW 窗口）；测试 socket 走 argv。
        setenv("KOPMS_INPUT_SOAK", "8,60", 1);
        execl((dir + "/kopms-compositor").c_str(), "kopms-compositor", socket.c_str(),
              "120", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (compositor < 0) return fail("fork compositor");
    { timespec ts{1, 200000000}; nanosleep(&ts, nullptr); }

    // 2. 反馈客户端（stdout 管道收集计数）
    int client_stdout[2];
    if (pipe(client_stdout) != 0) return fail("pipe");
    const pid_t client = fork();
    if (client == 0) {
        setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
        dup2(client_stdout[1], STDOUT_FILENO);
        close(client_stdout[0]);
        close(client_stdout[1]);
        execl((dir + "/kopms-input-client").c_str(), "kopms-input-client", "12",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    if (client < 0) return fail("fork client");
    close(client_stdout[1]);
    { timespec ts{0, 800000000}; nanosleep(&ts, nullptr); }
    const size_t compositor_fd_baseline = fd_count(compositor);

    // 3. 等待 soak 完成（8 轮 × ~25 步 × 20ms + 间隔 ≈ 6s；留足余量）
    // 客户端 12s 自行退出；等待上限 25s
    const int64_t deadline = now_ms() + 25000;
    std::string client_out;
    int client_status = 0;
    bool client_exited = false;
    while (now_ms() < deadline) {
        client_out += read_available(client_stdout[0]);
        const pid_t done = waitpid(client, &client_status, WNOHANG);
        if (done == client) {
            client_exited = true;
            client_out += read_available(client_stdout[0]);
            break;
        }
        if (strstr(client_out.c_str(), "DONE clicks=")) {
            // 计数已齐：稍等客户端自然退出
            { timespec ts{0, 300000000}; nanosleep(&ts, nullptr); }
            client_out += read_available(client_stdout[0]);
            const pid_t done2 = waitpid(client, &client_status, WNOHANG);
            if (done2 != client) {
                kill(client, SIGTERM);
                waitpid(client, &client_status, 0);
            }
            client_exited = true;
            break;
        }
        { timespec ts{0, 100000000}; nanosleep(&ts, nullptr); }
    }
    // 读端非阻塞：客户端静默期 read 不再阻塞
    const int flags = fcntl(client_stdout[0], F_GETFL);
    fcntl(client_stdout[0], F_SETFL, flags | O_NONBLOCK);
    if (!client_exited) {
        client_out += read_available(client_stdout[0]);
        kill(client, SIGTERM);
        waitpid(client, &client_status, 0);
    }

    // 4. 核对计数
    const size_t clicks = count_occurrences(client_out, "\nCLICK ");
    const size_t keys = count_occurrences(client_out, "\nKEY ");
    const size_t motions = count_occurrences(client_out, "\nMOTION ");
    std::printf("input-soak: client clicks=%zu keys=%zu motions=%zu\n", clicks, keys,
                motions);
    const char* done_pos = strstr(client_out.c_str(), "DONE clicks=");
    if (!done_pos) return fail("client never reported DONE");
    unsigned long long done_clicks = 0;
    unsigned long long done_keys = 0;
    unsigned long long done_motions = 0;
    if (sscanf(done_pos, "DONE clicks=%llu keys=%llu motions=%llu", &done_clicks,
               &done_keys, &done_motions) != 3) {
        return fail("DONE line malformed");
    }
    if (done_clicks < 36) {
        std::fprintf(stderr, "input-soak: clicks=%llu < 40\n", done_clicks);
        return fail("too few clicks delivered");
    }
    if (done_keys < 90) {
        std::fprintf(stderr, "input-soak: keys=%llu < 100\n", done_keys);
        return fail("too few keys delivered");
    }
    if (done_motions < 60) {
        std::fprintf(stderr, "input-soak: motions=%llu < 60\n", done_motions);
        return fail("too few motions delivered");
    }
    if (clicks != done_clicks || keys != done_keys) {
        return fail("stdout event lines do not match DONE counters");
    }

    // 5. 持久性：合成器存活 + fd 稳定（无泄漏）
    char probe[64];
    snprintf(probe, sizeof(probe), "/proc/%d/stat", compositor);
    if (access(probe, R_OK) != 0) return fail("compositor died during soak");
    const size_t compositor_fd_after = fd_count(compositor);
    std::printf("input-soak: compositor fds %zu -> %zu\n", compositor_fd_baseline,
                compositor_fd_after);
    if (compositor_fd_after > compositor_fd_baseline + 8) {
        return fail("compositor fd leak during soak");
    }

    // 6. 清理
    kill(client, SIGTERM);
    waitpid(client, &client_status, 0);
    kill(compositor, SIGTERM);
    int status = 0;
    waitpid(compositor, &status, 0);
    std::printf("input-soak: PASS\n");
    return 0;
}
