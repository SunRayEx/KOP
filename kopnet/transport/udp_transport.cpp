// KOPNET UDP 传输（数据报语义：保留边界、可能丢包/乱序）。
//
// 连接角色：socket 先 connect 到对端，后续 send/recv 固定 5 元组。
// 监听角色：绑定本地端口，首个报文到达时把 socket connect 到该对端
// （“会话化 UDP”），并把该首报文缓存在返回的 transport 中避免丢失。
#include "fd_transport.hpp"
#include "internal_factories.hpp"
#include "udp_socket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace kopnet {

class UdpTransport final : public FdTransport {
public:
    TransportSemantics semantics() const override { return TransportSemantics::Datagram; }
    bool supports_fds() const override { return false; }

    // 数据报传输不提供流式语义
    IoStatus read(uint8_t* /*buf*/, size_t /*cap*/, size_t* /*n*/, std::vector<int>* /*fds*/,
                  std::string* error) override {
        *error = "udp 传输不支持流式语义";
        return IoStatus::Error;
    }
    IoStatus write(const uint8_t* /*buf*/, size_t /*len*/, size_t* /*n*/,
                   const std::vector<int>* /*fds*/, std::string* error) override {
        *error = "udp 传输不支持流式语义";
        return IoStatus::Error;
    }

    IoStatus send_datagram(const uint8_t* data, size_t len, const std::vector<int>* /*fds*/,
                           std::string* error) override {
        if (len > KOPNET_MAX_DATAGRAM) {
            *error = "数据报超过最大长度";
            return IoStatus::Error;
        }
        ssize_t w = ::send(write_fd_, data, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("send 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        return IoStatus::Ok;
    }

    IoStatus recv_datagram(std::vector<uint8_t>* data, std::vector<int>* /*fds*/,
                           std::string* error) override {
        if (!pending_.empty()) {
            *data = std::move(pending_);
            return IoStatus::Ok;
        }
        uint8_t buf[KOPNET_MAX_DATAGRAM];
        ssize_t r = ::recv(read_fd_, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("recv 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        data->assign(buf, buf + r);
        return IoStatus::Ok;
    }

    void set_pending(std::vector<uint8_t> pkt) { pending_ = std::move(pkt); }

private:
    std::vector<uint8_t> pending_;
};

int make_udp_socket(const std::string& host, uint16_t port, bool bind_local,
                    std::string* error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = bind_local ? (AI_PASSIVE | AI_NUMERICSERV) : AI_NUMERICSERV;
    const char* node = (bind_local && (host.empty() || host == "*")) ? nullptr : host.c_str();
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
        if (bind_local) {
            if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
                ::close(fd);
                fd = -1;
                continue;
            }
        } else if (port != 0) {
            // 客户端先绑定本地任意端口（显式 local_port 参数时才有意义）
        }
        break;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        *error = "无法创建 udp socket " + host + ":" + port_str;
        return -1;
    }
    return fd;
}

int resolve_and_connect(int fd, const std::string& host, uint16_t port,
                        std::string* error) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        *error = std::string("getaddrinfo 失败: ") + ::gai_strerror(rc);
        return -1;
    }
    int ok = -1;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            ok = 0;
            break;
        }
    }
    ::freeaddrinfo(res);
    if (ok != 0) {
        *error = "udp connect 失败: " + host + ":" + port_str;
    }
    return ok;
}

namespace {

class UdpListener final : public TransportListener {
public:
    ~UdpListener() override { close(); }

    bool init(const std::string& host, uint16_t port, std::string* error) {
        fd_ = make_udp_socket(host, port, true, error);
        if (fd_ < 0) return false;
        desc_ = "udp-listen:" + host + ":" + std::to_string(port);
        return true;
    }

    bool valid() const override { return fd_ >= 0; }

    IoStatus accept(std::unique_ptr<Transport>* out, int timeout_ms,
                    std::string* error) override {
        if (fd_ < 0) {
            // 数据报监听器是单会话模型：首个报文 connect 锁定五元组后 fd 所有权
            // 已移交。此后不再有新会话可给，按“暂无连接”休眠一个周期返回，
            // 避免调用方（TunnelServer::run）因 Closed 忙转并刷告警。
            std::this_thread::sleep_for(
                std::chrono::milliseconds(timeout_ms < 0 ? 100 : timeout_ms));
            return IoStatus::WouldBlock;
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
        // 收首个报文并记录对端
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        uint8_t buf[KOPNET_MAX_DATAGRAM];
        ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                               reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("recvfrom 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&peer), peer_len) != 0) {
            *error = std::string("udp 会话化 connect 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        auto t = std::make_unique<UdpTransport>();
        t->set_pending(std::vector<uint8_t>(buf, buf + n));
        t->reset(fd_, fd_, false, desc_ + "+session");  // fd 所有权移交给 transport
        fd_ = -1;
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

std::unique_ptr<Transport> udp_connect(const std::string& host, uint16_t port,
                                       std::string* error) {
    int fd = make_udp_socket(host, port, false, error);
    if (fd < 0) return nullptr;
    if (resolve_and_connect(fd, host, port, error) != 0) {
        ::close(fd);
        return nullptr;
    }
    auto t = std::make_unique<UdpTransport>();
    t->reset(fd, fd, true, "udp:" + host + ":" + std::to_string(port));
    return t;
}

std::unique_ptr<TransportListener> udp_listen(const std::string& host, uint16_t port,
                                              std::string* error) {
    auto l = std::make_unique<UdpListener>();
    if (!l->init(host, port, error)) return nullptr;
    return l;
}

}  // namespace kopnet
