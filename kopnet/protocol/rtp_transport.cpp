// KOPNET RTP 适配器实现：UDP socket + RTP 分帧（详见 rtp_transport.hpp）。
#include "rtp_transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#include "transport/udp_socket.hpp"

#include "kop/log.h"

namespace kopnet {

namespace {

void write_u16_be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[1] = static_cast<uint8_t>(v & 0xff);
}

void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<uint8_t>(v & 0xff);
}

uint32_t random_u32() {
    std::random_device rd;
    std::mt19937 gen(rd());
    return static_cast<uint32_t>(gen());
}

IoStatus poll_fd(int fd, short events, int timeout_ms, std::string* error) {
    if (fd < 0) {
        *error = "rtp transport 已关闭";
        return IoStatus::Closed;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
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
        *error = "poll 报告 POLLERR/POLLNVAL";
        return IoStatus::Error;
    }
    if (pfd.revents & POLLHUP) return IoStatus::Closed;
    if (pfd.revents & events) return IoStatus::Ok;
    return IoStatus::WouldBlock;
}

}  // namespace

void RtpTransport::attach(int fd, std::vector<uint8_t> pending, RtpConfig config,
                          std::string desc) {
    close();
    fd_ = fd;
    pending_ = std::move(pending);
    config_ = config;
    desc_ = std::move(desc);
    // RFC 3550：序号与时间戳取随机初值，SSRC 未指定时也随机生成。
    if (config_.ssrc == 0) config_.ssrc = random_u32();
    if (send_seq_ == 0) send_seq_ = static_cast<uint16_t>(random_u32());
    if (send_ts_ == 0) send_ts_ = random_u32();
    send_ts_step_ = config_.ts_step != 0
                        ? config_.ts_step
                        : (config_.fps > 0 ? config_.clock_rate / config_.fps
                                           : config_.clock_rate / 30);
}

void RtpTransport::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    pending_.clear();
}

IoStatus RtpTransport::read(uint8_t*, size_t, size_t*, std::vector<int>*,
                            std::string* error) {
    *error = "rtp 传输不支持流式语义";
    return IoStatus::Error;
}

IoStatus RtpTransport::write(const uint8_t*, size_t, size_t*, const std::vector<int>*,
                             std::string* error) {
    *error = "rtp 传输不支持流式语义";
    return IoStatus::Error;
}

IoStatus RtpTransport::send_datagram(const uint8_t* data, size_t len,
                                     const std::vector<int>* fds, std::string* error) {
    if (fd_ < 0) {
        *error = "rtp transport 已关闭";
        return IoStatus::Error;
    }
    if (fds && !fds->empty()) {
        *error = "rtp 传输不能透传 fd";
        return IoStatus::Error;
    }
    // 12 字节固定头 + 负载；不分片，超限直接拒绝。
    if (len + 12 > KOPNET_MAX_DATAGRAM) {
        *error = "rtp 负载超过数据报上限，需要上层分片";
        return IoStatus::Error;
    }
    if (len > 1400) {
        // 典型以太网 MTU 1500 - 12 头 - 20 IP/8 UDP 后的安全值
        KOP_LOG_WARN("kopnet", "rtp 负载 %zu 字节超过常见 MTU 安全值，可能被分片", len);
    }

    uint8_t hdr[12];
    hdr[0] = 0x80;  // V=2, P=0, X=0, CC=0
    hdr[1] = static_cast<uint8_t>((config_.marker ? 0x80 : 0x00) |
                                  (config_.payload_type & 0x7f));
    write_u16_be(hdr + 2, send_seq_);
    write_u32_be(hdr + 4, send_ts_);
    write_u32_be(hdr + 8, config_.ssrc);

    iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = const_cast<uint8_t*>(data);
    iov[1].iov_len = len;
    msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = len > 0 ? 2 : 1;
    ssize_t w = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
        *error = std::string("rtp send 失败: ") + std::strerror(errno);
        return IoStatus::Error;
    }
    send_seq_ = static_cast<uint16_t>(send_seq_ + 1);
    send_ts_ += send_ts_step_;
    ++stats_.sent;
    return IoStatus::Ok;
}

IoStatus RtpTransport::recv_datagram(std::vector<uint8_t>* data, std::vector<int>* fds,
                                     std::string* error) {
    if (fd_ < 0) {
        *error = "rtp transport 已关闭";
        return IoStatus::Error;
    }
    if (fds) fds->clear();

    // listen 侧缓存的首包优先处理
    if (!pending_.empty()) {
        std::vector<uint8_t> first = std::move(pending_);
        pending_.clear();
        return handle_packet(first.data(), first.size(), data, error);
    }

    uint8_t buf[KOPNET_MAX_DATAGRAM];
    ssize_t r = ::recv(fd_, buf, sizeof(buf), 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
        *error = std::string("rtp recv 失败: ") + std::strerror(errno);
        return IoStatus::Error;
    }
    return handle_packet(buf, static_cast<size_t>(r), data, error);
}

IoStatus RtpTransport::handle_packet(const uint8_t* pkt, size_t len,
                                     std::vector<uint8_t>* data, std::string* error) {
    const MediaPacketKind kind = classify_media_packet(pkt, len);
    if (kind != MediaPacketKind::Rtp) {
        if (kind == MediaPacketKind::Rtcp) {
            ++stats_.rtcp_skipped;
        } else {
            ++stats_.other_skipped;
            KOP_LOG_WARN("kopnet", "rtp 端口收到非 RTP 报文 (%zu 字节)，已跳过", len);
        }
        // 非媒体报文不算数据到达：回到等待。调用方应继续 wait_readable。
        data->clear();
        return IoStatus::WouldBlock;
    }

    RtpHeader hdr;
    size_t off = 0;
    if (!parse_rtp(pkt, len, &hdr, &off, error)) {
        ++stats_.other_skipped;
        *error = std::string("rtp 解析失败: ") + *error;
        return IoStatus::Error;
    }
    if (peer_ssrc_ == 0) peer_ssrc_ = hdr.ssrc;

    // 16bit 回绕的带符号序号差：锁定基准后统计丢包/重复。
    if (!recv_synced_) {
        recv_synced_ = true;
        recv_expected_ = static_cast<uint16_t>(hdr.sequence + 1);
    } else {
        const int32_t delta =
            static_cast<int32_t>(static_cast<int16_t>(hdr.sequence - recv_expected_));
        if (delta > 0) {
            // 前向跳跃：[expected, sequence) 之间的包未按序到达即计为丢失。
            stats_.lost += static_cast<uint64_t>(delta);
            recv_expected_ = static_cast<uint16_t>(hdr.sequence + 1);
        } else if (delta == 0) {
            recv_expected_ = static_cast<uint16_t>(hdr.sequence + 1);
        } else {
            ++stats_.duplicates;
        }
    }

    // padding 位的最后一个字节是填充长度（含自身）。
    size_t payload_len = len - off;
    if (hdr.padding && payload_len > 0) {
        const uint8_t pad = pkt[len - 1];
        if (pad >= 1 && pad <= payload_len) payload_len -= pad;
    }
    data->assign(pkt + off, pkt + off + payload_len);
    ++stats_.received;
    return IoStatus::Ok;
}

IoStatus RtpTransport::wait_readable(int timeout_ms, std::string* error) {
    return poll_fd(fd_, POLLIN, timeout_ms, error);
}

IoStatus RtpTransport::wait_writable(int timeout_ms, std::string* error) {
    return poll_fd(fd_, POLLOUT, timeout_ms, error);
}

namespace {

// 会话化 UDP 监听器：复用 UDP 的 bind + 首报文 connect 逻辑，但产出
// RtpTransport（首包作为 pending 一并交给 RtpTransport 处理）。
class RtpListener final : public TransportListener {
public:
    ~RtpListener() override { close(); }

    bool init(const std::string& host, uint16_t port, RtpConfig config,
              std::string* error) {
        fd_ = make_udp_socket(host, port, true, error);
        if (fd_ < 0) return false;
        config_ = config;
        desc_ = "rtp-listen:" + host + ":" + std::to_string(port);
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
        const IoStatus st = poll_fd(fd_, POLLIN, timeout_ms, error);
        if (st != IoStatus::Ok) return st;

        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        uint8_t buf[KOPNET_MAX_DATAGRAM];
        const ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                                     reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("recvfrom 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&peer), peer_len) != 0) {
            *error = std::string("rtp 会话化 connect 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        auto t = std::make_unique<RtpTransport>();
        t->attach(fd_, std::vector<uint8_t>(buf, buf + n), config_, desc_ + "+session");
        fd_ = -1;  // 所有权移交
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
    RtpConfig config_{};
    std::string desc_;
};

}  // namespace

std::unique_ptr<Transport> rtp_connect(const std::string& host, uint16_t port,
                                       const RtpConfig& config, std::string* error) {
    int fd = make_udp_socket(host, port, false, error);
    if (fd < 0) return nullptr;
    if (resolve_and_connect(fd, host, port, error) != 0) {
        ::close(fd);
        return nullptr;
    }
    auto t = std::make_unique<RtpTransport>();
    t->attach(fd, {}, config, "rtp:" + host + ":" + std::to_string(port));
    return t;
}

std::unique_ptr<TransportListener> rtp_listen(const std::string& host, uint16_t port,
                                              const RtpConfig& config, std::string* error) {
    auto l = std::make_unique<RtpListener>();
    if (!l->init(host, port, config, error)) return nullptr;
    return l;
}

}  // namespace kopnet
