// KOPNET 管道传输：把一个子进程的 stdin/stdout 接成流式 Transport。
//
// SSH adapter 用它把 `ssh host kopnet-relay` 的 stdio 变成 KOPNET 隧道的
// 承载通路——无需在进程内嵌入 SSH 加密栈，即可获得加密的远程控制通道
// （与 git 的 ext:: / mosh 的 SSH 引导同一路数）。
#include "fd_transport.hpp"
#include "internal_factories.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace kopnet {

class PipeTransport final : public FdTransport {
public:
    TransportSemantics semantics() const override { return TransportSemantics::Stream; }
    bool supports_fds() const override { return false; }

    IoStatus read(uint8_t* buf, size_t cap, size_t* n, std::vector<int>* /*fds*/,
                  std::string* error) override {
        return raw_recv(buf, cap, n, error);
    }
    IoStatus write(const uint8_t* buf, size_t len, size_t* n,
                   const std::vector<int>* /*fds*/, std::string* error) override {
        return raw_send(buf, len, n, error);
    }
    IoStatus send_datagram(const uint8_t* /*data*/, size_t /*len*/,
                           const std::vector<int>* /*fds*/, std::string* error) override {
        *error = "管道传输不支持数据报语义";
        return IoStatus::Error;
    }
    IoStatus recv_datagram(std::vector<uint8_t>* /*data*/, std::vector<int>* /*fds*/,
                           std::string* error) override {
        *error = "管道传输不支持数据报语义";
        return IoStatus::Error;
    }

    void set_child(pid_t pid) { child_ = pid; }

    // 关闭并回收子进程（SIGTERM 后 wait，避免僵尸进程）
    void shutdown_child() {
        if (child_ > 0) {
            ::kill(child_, SIGTERM);
            int status = 0;
            // 有界等待，超时则 SIGKILL
            for (int i = 0; i < 50; ++i) {
                pid_t r = ::waitpid(child_, &status, WNOHANG);
                if (r == child_ || r < 0) {
                    child_ = -1;
                    return;
                }
                ::usleep(10000);
            }
            ::kill(child_, SIGKILL);
            ::waitpid(child_, &status, 0);
            child_ = -1;
        }
    }

    ~PipeTransport() override { shutdown_child(); }

private:
    pid_t child_ = -1;
};

// fork + exec 一个子进程，把其 stdin/stdout 接成 PipeTransport。
// argv[0] 为程序名。失败返回 nullptr。
std::unique_ptr<Transport> spawn_pipe_process(const std::vector<std::string>& argv,
                                              std::string* error) {
    ignore_sigpipe();
    if (argv.empty()) {
        *error = "spawn 参数为空";
        return nullptr;
    }
    int in_pipe[2];    // 父写 → 子 stdin
    int out_pipe[2];   // 子 stdout → 父读
    if (::pipe(in_pipe) < 0 || ::pipe(out_pipe) < 0) {
        *error = std::string("pipe 失败: ") + std::strerror(errno);
        return nullptr;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        *error = std::string("fork 失败: ") + std::strerror(errno);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return nullptr;
    }
    if (pid == 0) {
        // 子进程
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        // stderr 继承（诊断可见）
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        // exec 失败
        std::string msg = "execvp 失败: " + std::string(std::strerror(errno));
        ssize_t w = ::write(STDERR_FILENO, msg.c_str(), msg.size());
        (void)w;
        ::_exit(127);
    }
    // 父进程
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    // 非阻塞
    int flags = ::fcntl(in_pipe[1], F_GETFL, 0);
    ::fcntl(in_pipe[1], F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(out_pipe[0], F_GETFL, 0);
    ::fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    auto t = std::make_unique<PipeTransport>();
    t->reset(out_pipe[0], in_pipe[1], true, "pipe:" + argv[0]);
    t->set_child(pid);
    return t;
}

std::unique_ptr<Transport> make_stdio_transport(std::string* error) {
    // stdin/stdout 置为非阻塞（隧道工作线程以 poll + 非阻塞 I/O 驱动）
    for (int fd : {STDIN_FILENO, STDOUT_FILENO}) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            *error = std::string("stdio 非阻塞设置失败: ") + std::strerror(errno);
            return nullptr;
        }
    }
    auto t = std::make_unique<PipeTransport>();
    t->reset(STDIN_FILENO, STDOUT_FILENO, /*owns=*/false, "stdio");
    (void)error;
    return t;
}

}  // namespace kopnet
