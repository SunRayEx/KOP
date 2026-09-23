// KOPNET AF_UNIX 传输（SOCK_STREAM，支持 SCM_RIGHTS fd 透传）。
#include "fd_transport.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

#include "kop/log.h"
#include "kopnet/endpoint.hpp"
#include "kopnet/transport.hpp"
#include "kopnet/transport_listener.hpp"

namespace kopnet {

bool resolve_unix_path(const std::string& name_or_path, std::string* path,
                       std::string* error) {
    if (name_or_path.empty()) {
        *error = "unix 端点名字为空";
        return false;
    }
    if (name_or_path[0] == '/') {
        *path = name_or_path;
        return true;
    }
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg == nullptr || xdg[0] == '\0') {
        *error = "相对 unix 名需要 XDG_RUNTIME_DIR";
        return false;
    }
    *path = std::string(xdg) + "/" + name_or_path;
    return true;
}

class UnixStreamTransport final : public FdTransport {
public:
    TransportSemantics semantics() const override { return TransportSemantics::Stream; }
    bool supports_fds() const override { return true; }

    IoStatus read(uint8_t* buf, size_t cap, size_t* n, std::vector<int>* fds,
                  std::string* error) override {
        return recv_with_fds(buf, cap, n, fds, error);
    }
    IoStatus write(const uint8_t* buf, size_t len, size_t* n, const std::vector<int>* fds,
                   std::string* error) override {
        return send_with_fds(buf, len, n, fds, error);
    }
    IoStatus send_datagram(const uint8_t* data, size_t len, const std::vector<int>* fds,
                           std::string* error) override {
        *error = "unix stream 传输不支持数据报语义";
        return IoStatus::Error;
    }
    IoStatus recv_datagram(std::vector<uint8_t>* data, std::vector<int>* fds,
                           std::string* error) override {
        *error = "unix stream 传输不支持数据报语义";
        return IoStatus::Error;
    }
};

class UnixListener final : public TransportListener {
public:
    ~UnixListener() override { close(); }

    bool bind(const std::string& name_or_path, std::string* error) {
        std::string path;
        if (!resolve_unix_path(name_or_path, &path, error)) return false;
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            *error = std::string("socket 失败: ") + std::strerror(errno);
            return false;
        }
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            *error = "unix socket 路径过长";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        // 复用已存在的 socket 文件（bind 前清理）
        ::unlink(path.c_str());
        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            *error = std::string("bind 失败: ") + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (::listen(fd_, 16) < 0) {
            *error = std::string("listen 失败: ") + std::strerror(errno);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        path_ = path;
        desc_ = "unix-listen:" + path;
        return true;
    }

    bool valid() const override { return fd_ >= 0; }

    IoStatus accept(std::unique_ptr<Transport>* out, int timeout_ms,
                    std::string* error) override {
        if (fd_ < 0) {
            *error = "监听器已关闭";
            return IoStatus::Closed;
        }
        IoStatus ws = wait_readable(timeout_ms, error);
        if (ws != IoStatus::Ok) return ws;
        int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("accept4 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        auto t = std::make_unique<UnixStreamTransport>();
        t->reset(cfd, cfd, true, "unix:" + path_);
        *out = std::move(t);
        return IoStatus::Ok;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (!path_.empty()) {
            ::unlink(path_.c_str());
            path_.clear();
        }
    }

    std::string describe() const override { return desc_; }

private:
    int fd_ = -1;
    std::string path_;
    std::string desc_;
    // 复用 fd 等待（监听 fd 可读 = 有连接）
    IoStatus wait_readable(int timeout_ms, std::string* error) {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r;
        do {
            r = ::poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            *error = std::string("poll 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        if (r == 0) return IoStatus::WouldBlock;
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            *error = "监听器 poll POLLERR";
            return IoStatus::Error;
        }
        return IoStatus::Ok;
    }
};

bool make_socketpair_transports(std::unique_ptr<Transport>* a, std::unique_ptr<Transport>* b,
                                std::string* error) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
        *error = std::string("socketpair 失败: ") + std::strerror(errno);
        return false;
    }
    auto ta = std::make_unique<UnixStreamTransport>();
    ta->reset(fds[0], fds[0], true, "socketpair:a");
    auto tb = std::make_unique<UnixStreamTransport>();
    tb->reset(fds[1], fds[1], true, "socketpair:b");
    *a = std::move(ta);
    *b = std::move(tb);
    return true;
}

// 拨号一个 AF_UNIX 连接（供 adapter 工厂使用）。
std::unique_ptr<Transport> unix_connect(const std::string& name_or_path,
                                        std::string* error) {
    std::string path;
    if (!resolve_unix_path(name_or_path, &path, error)) return nullptr;
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::string("socket 失败: ") + std::strerror(errno);
        return nullptr;
    }
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        *error = "unix socket 路径过长";
        ::close(fd);
        return nullptr;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            *error = std::string("connect 失败: ") + std::strerror(errno);
            ::close(fd);
            return nullptr;
        }
        // 非阻塞连接：等待可写并检查 SO_ERROR
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int r;
        do {
            r = ::poll(&pfd, 1, 5000);
        } while (r < 0 && errno == EINTR);
        if (r <= 0) {
            *error = "unix connect 超时";
            ::close(fd);
            return nullptr;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
            *error = std::string("unix connect SO_ERROR: ") + std::strerror(soerr);
            ::close(fd);
            return nullptr;
        }
    }
    auto t = std::make_unique<UnixStreamTransport>();
    t->reset(fd, fd, true, "unix:" + path);
    return t;
}

std::unique_ptr<TransportListener> unix_listen(const std::string& name_or_path,
                                               std::string* error) {
    auto l = std::make_unique<UnixListener>();
    if (!l->bind(name_or_path, error)) return nullptr;
    return l;
}

}  // namespace kopnet
