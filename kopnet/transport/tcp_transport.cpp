// KOPNET TCP 传输（可靠有序字节流，无 fd 透传）。
#include "fd_transport.hpp"
#include "internal_factories.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

namespace kopnet {

namespace {

// 阻塞式解析并连接（getaddrinfo + 非阻塞 connect + SO_ERROR 检查）。
int dial_tcp(const std::string& host, uint16_t port, std::string* error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        *error = std::string("getaddrinfo 失败: ") + ::gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int r;
            do {
                r = ::poll(&pfd, 1, 5000);
            } while (r < 0 && errno == EINTR);
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (r > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 &&
                soerr == 0) {
                break;
            }
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        *error = "无法连接 " + host + ":" + port_str;
        return -1;
    }
    return fd;
}

int listen_tcp(const std::string& host, uint16_t port, std::string* error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    const char* node = host.empty() || host == "*" ? nullptr : host.c_str();
    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(node, port_str.c_str(), &hints, &res);
    if (rc != 0) {
        *error = std::string("getaddrinfo 失败: ") + ::gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 16) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        *error = "无法监听 " + host + ":" + port_str;
        return -1;
    }
    return fd;
}

class TcpTransport final : public FdTransport {
public:
    TransportSemantics semantics() const override { return TransportSemantics::Stream; }
    bool supports_fds() const override { return false; }

    IoStatus read(uint8_t* buf, size_t cap, size_t* n, std::vector<int>* /*fds*/,
                  std::string* error) override {
        return raw_recv(buf, cap, n, error);
    }
    IoStatus write(const uint8_t* buf, size_t len, size_t* n,
                   const std::vector<int>* /*fds*/, std::string* error) override {
        return raw_send_socket(buf, len, n, error);
    }
    IoStatus send_datagram(const uint8_t* /*data*/, size_t /*len*/,
                           const std::vector<int>* /*fds*/, std::string* error) override {
        *error = "tcp 传输不支持数据报语义";
        return IoStatus::Error;
    }
    IoStatus recv_datagram(std::vector<uint8_t>* /*data*/, std::vector<int>* /*fds*/,
                           std::string* error) override {
        *error = "tcp 传输不支持数据报语义";
        return IoStatus::Error;
    }
};

class TcpListener final : public TransportListener {
public:
    ~TcpListener() override { close(); }

    bool init(const std::string& host, uint16_t port, std::string* error) {
        fd_ = listen_tcp(host, port, error);
        if (fd_ < 0) return false;
        desc_ = "tcp-listen:" + host + ":" + std::to_string(port);
        return true;
    }

    bool valid() const override { return fd_ >= 0; }

    IoStatus accept(std::unique_ptr<Transport>* out, int timeout_ms,
                    std::string* error) override {
        if (fd_ < 0) {
            *error = "监听器已关闭";
            return IoStatus::Closed;
        }
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
            *error = "tcp 监听器 POLLERR";
            return IoStatus::Error;
        }
        int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("accept4 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        auto t = std::make_unique<TcpTransport>();
        t->reset(cfd, cfd, true, desc_ + "+conn");
        *out = std::move(t);
        return IoStatus::Ok;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string describe() const override { return desc_; }

private:
    int fd_ = -1;
    std::string desc_;
};

}  // namespace

std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port,
                                       std::string* error) {
    int fd = dial_tcp(host, port, error);
    if (fd < 0) return nullptr;
    auto t = std::make_unique<TcpTransport>();
    t->reset(fd, fd, true, "tcp:" + host + ":" + std::to_string(port));
    return t;
}

std::unique_ptr<TransportListener> tcp_listen(const std::string& host, uint16_t port,
                                              std::string* error) {
    auto l = std::make_unique<TcpListener>();
    if (!l->init(host, port, error)) return nullptr;
    return l;
}

}  // namespace kopnet
