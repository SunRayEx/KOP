// KOPNET 传输内部基类：基于 fd 的 poll/非阻塞读写与 SCM_RIGHTS 收发。
// 仅供传输实现使用，不对外暴露（不在 include/kopnet 下）。
#pragma once

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kop/log.h"
#include "kopnet/transport.hpp"

namespace kopnet {

class FdTransport : public Transport {
public:
    FdTransport() = default;
    ~FdTransport() override { close(); }

    FdTransport(const FdTransport&) = delete;
    FdTransport& operator=(const FdTransport&) = delete;
    FdTransport(FdTransport&&) = delete;
    FdTransport& operator=(FdTransport&&) = delete;

    // socket 传输：read_fd == write_fd。管道传输：两者不同。
    void reset(int read_fd, int write_fd, bool owns, std::string desc) {
        close();
        read_fd_ = read_fd;
        write_fd_ = write_fd;
        owns_ = owns;
        desc_ = std::move(desc);
    }

    bool valid() const override { return read_fd_ >= 0 || write_fd_ >= 0; }

    void close() override {
        if (owns_) {
            if (read_fd_ >= 0) ::close(read_fd_);
            if (write_fd_ >= 0 && write_fd_ != read_fd_) ::close(write_fd_);
        }
        read_fd_ = write_fd_ = -1;
    }

    std::string describe() const override { return desc_; }

    IoStatus wait_readable(int timeout_ms, std::string* error) override {
        return poll_fd(read_fd_, POLLIN, timeout_ms, error);
    }

    IoStatus wait_writable(int timeout_ms, std::string* error) override {
        return poll_fd(write_fd_, POLLOUT, timeout_ms, error);
    }

    // 原始 recv（无附属 fd）
    IoStatus raw_recv(uint8_t* buf, size_t cap, size_t* n, std::string* error) {
        ssize_t r = ::read(read_fd_, buf, cap);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("read 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        if (r == 0) return IoStatus::Closed;
        *n = static_cast<size_t>(r);
        return IoStatus::Ok;
    }

    // socket 专用 send：MSG_NOSIGNAL 防止写半连接 socket 时 SIGPIPE 杀进程
    IoStatus raw_send_socket(const uint8_t* buf, size_t len, size_t* n,
                             std::string* error) {
        ssize_t w = ::send(write_fd_, buf, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("send 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        *n = static_cast<size_t>(w);
        return IoStatus::Ok;
    }

    // 原始 send（管道：write 不接受 flags，SIGPIPE 由库入口统一忽略）
    IoStatus raw_send(const uint8_t* buf, size_t len, size_t* n, std::string* error) {
        ssize_t w = ::write(write_fd_, buf, len);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("write 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        *n = static_cast<size_t>(w);
        return IoStatus::Ok;
    }

    // SCM_RIGHTS 收（仅 AF_UNIX socket）
    IoStatus recv_with_fds(uint8_t* buf, size_t cap, size_t* n, std::vector<int>* fds,
                           std::string* error) {
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = cap;
        char cmsgbuf[CMSG_SPACE(sizeof(int) * KOPNET_MAX_FDS_PER_FRAME)];
        std::memset(cmsgbuf, 0, sizeof(cmsgbuf));
        struct msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        ssize_t r = ::recvmsg(read_fd_, &msg, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("recvmsg 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        if (r == 0) return IoStatus::Closed;
        *n = static_cast<size_t>(r);
        if (fds) {
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
                 cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                    int count = static_cast<int>(
                        (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    for (int i = 0; i < count; ++i) {
                        int fd;
                        std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                        fds->push_back(fd);
                    }
                }
            }
        }
        return IoStatus::Ok;
    }

    // SCM_RIGHTS 发（仅 AF_UNIX socket）
    IoStatus send_with_fds(const uint8_t* buf, size_t len, size_t* n,
                           const std::vector<int>* fds, std::string* error) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(buf);
        iov.iov_len = len;
        char cmsgbuf[CMSG_SPACE(sizeof(int) * KOPNET_MAX_FDS_PER_FRAME)];
        std::memset(cmsgbuf, 0, sizeof(cmsgbuf));
        struct msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        size_t fd_count = (fds && fds->size() <= KOPNET_MAX_FDS_PER_FRAME) ? fds->size() : 0;
        if (fd_count > 0) {
            msg.msg_control = cmsgbuf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
            std::memcpy(CMSG_DATA(cmsg), fds->data(), sizeof(int) * fd_count);
        }
        ssize_t w = ::sendmsg(write_fd_, &msg, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
            *error = std::string("sendmsg 失败: ") + std::strerror(errno);
            return IoStatus::Error;
        }
        *n = static_cast<size_t>(w);
        return IoStatus::Ok;
    }

protected:
    int read_fd_ = -1;
    int write_fd_ = -1;
    bool owns_ = true;
    std::string desc_;

private:
    static IoStatus poll_fd(int fd, short events, int timeout_ms, std::string* error) {
        if (fd < 0) {
            *error = "transport 已关闭";
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
};

}  // namespace kopnet
