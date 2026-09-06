#include "bus2layer.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>

namespace kopms {

namespace {

void close_fds(std::vector<int>* fds) {
    if (!fds) return;
    for (int fd : *fds) {
        if (fd >= 0) ::close(fd);
    }
    fds->clear();
}

void set_error_errno(std::string* error, const char* operation) {
    if (!error) return;
    *error = std::string(operation) + ": " + std::strerror(errno);
}

bool set_cloexec(int fd, std::string* error) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        set_error_errno(error, "fcntl(F_GETFD)");
        return false;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        set_error_errno(error, "fcntl(F_SETFD)");
        return false;
    }
    return true;
}

bool valid_socket_name(const std::string& path, std::string* error) {
    if (path.empty() ||
        path.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path)) {
        if (error) *error = "Unix socket path is empty or too long";
        return false;
    }
    return true;
}

}  // namespace

BusMessage::~BusMessage() { clear(); }

BusMessage::BusMessage(BusMessage&& other) noexcept
    : header(other.header), payload(std::move(other.payload)), fds(std::move(other.fds)) {
    other.header = {};
}

BusMessage& BusMessage::operator=(BusMessage&& other) noexcept {
    if (this == &other) return *this;
    clear();
    header = other.header;
    payload = std::move(other.payload);
    fds = std::move(other.fds);
    other.header = {};
    return *this;
}

void BusMessage::clear() {
    close_fds(&fds);
    payload.clear();
    header = {};
}

std::vector<int> BusMessage::take_fds() { return std::move(fds); }

BusConnection::~BusConnection() { close(); }

BusConnection::BusConnection(BusConnection&& other) noexcept
    : fd_(other.fd_), owns_fd_(other.owns_fd_) {
    other.fd_ = -1;
    other.owns_fd_ = true;
}

BusConnection& BusConnection::operator=(BusConnection&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = other.fd_;
    owns_fd_ = other.owns_fd_;
    other.fd_ = -1;
    other.owns_fd_ = true;
    return *this;
}

void BusConnection::reset(int fd, bool owns_fd) {
    close();
    fd_ = fd;
    owns_fd_ = owns_fd;
}

void BusConnection::close() {
    if (fd_ >= 0 && owns_fd_) ::close(fd_);
    fd_ = -1;
    owns_fd_ = true;
}

bool BusConnection::set_nonblocking(bool enabled, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "invalid BUS connection";
        return false;
    }
    const int old_flags = fcntl(fd_, F_GETFL);
    if (old_flags < 0) {
        set_error_errno(error, "fcntl(F_GETFL)");
        return false;
    }
    const int new_flags = enabled ? old_flags | O_NONBLOCK : old_flags & ~O_NONBLOCK;
    if (fcntl(fd_, F_SETFL, new_flags) < 0) {
        set_error_errno(error, "fcntl(F_SETFL)");
        return false;
    }
    return true;
}

bool BusConnection::send_message(uint16_t major, uint16_t minor, uint16_t type,
                                  uint16_t flags, uint64_t sequence,
                                  const std::vector<uint8_t>& payload,
                                  const std::vector<int>& fds, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "invalid BUS connection";
        return false;
    }
    if (payload.size() > KOPMS_PROTOCOL_MAX_PAYLOAD) {
        if (error) *error = "BUS payload exceeds the limit";
        return false;
    }
    if (fds.size() > KOPMS_PROTOCOL_MAX_FDS) {
        if (error) *error = "BUS fd count exceeds the limit";
        return false;
    }
    for (int fd : fds) {
        if (fd < 0) {
            if (error) *error = "BUS cannot send an invalid fd";
            return false;
        }
    }

    KopmsMessageHeader header{};
    header.magic = KOPMS_PROTOCOL_MAGIC;
    header.major = major;
    header.minor = minor;
    header.type = type;
    header.flags = flags;
    header.sequence = sequence;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.fd_count = static_cast<uint32_t>(fds.size());
    WireHeader wire_header{};
    encode_message_header(header, &wire_header);

    iovec iov[2]{};
    iov[0].iov_base = wire_header.data();
    iov[0].iov_len = wire_header.size();
    iov[1].iov_base = payload.empty() ? nullptr : const_cast<uint8_t*>(payload.data());
    iov[1].iov_len = payload.size();

    std::array<uint8_t, CMSG_SPACE(sizeof(int) * KOPMS_PROTOCOL_MAX_FDS)> control{};
    msghdr message{};
    message.msg_iov = iov;
    message.msg_iovlen = payload.empty() ? 1 : 2;
    if (!fds.empty()) {
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
        message.msg_controllen = cmsg->cmsg_len;
    }

    const ssize_t expected = static_cast<ssize_t>(wire_header.size() + payload.size());
    const ssize_t sent = sendmsg(fd_, &message, MSG_NOSIGNAL);
    if (sent != expected) {
        if (sent < 0) {
            set_error_errno(error, "sendmsg");
        } else if (error) {
            *error = "sendmsg sent a partial BUS packet";
        }
        return false;
    }
    return true;
}

BusReceiveStatus BusConnection::receive(BusMessage* message, std::string* error) {
    if (!message) {
        if (error) *error = "BUS receive target is null";
        return BusReceiveStatus::Error;
    }
    message->clear();
    if (fd_ < 0) {
        if (error) *error = "invalid BUS connection";
        return BusReceiveStatus::Error;
    }

    WireHeader wire_header{};
    std::vector<uint8_t> payload(KOPMS_PROTOCOL_MAX_PAYLOAD);
    std::array<uint8_t, CMSG_SPACE(sizeof(int) * KOPMS_PROTOCOL_MAX_FDS)> control{};
    iovec iov[2]{};
    iov[0].iov_base = wire_header.data();
    iov[0].iov_len = wire_header.size();
    iov[1].iov_base = payload.data();
    iov[1].iov_len = payload.size();
    msghdr raw{};
    raw.msg_iov = iov;
    raw.msg_iovlen = 2;
    raw.msg_control = control.data();
    raw.msg_controllen = control.size();

    int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t received = recvmsg(fd_, &raw, flags);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return BusReceiveStatus::WouldBlock;
        if (errno == EINTR) return BusReceiveStatus::WouldBlock;
        set_error_errno(error, "recvmsg");
        return BusReceiveStatus::Error;
    }
    if (received == 0) return BusReceiveStatus::Closed;

    std::vector<int> received_fds;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&raw); cmsg;
         cmsg = CMSG_NXTHDR(&raw, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            close_fds(&received_fds);
            if (error) *error = "malformed SCM_RIGHTS control message";
            return BusReceiveStatus::Error;
        }
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        const size_t count = bytes / sizeof(int);
        if (bytes % sizeof(int) != 0 || count > KOPMS_PROTOCOL_MAX_FDS) {
            close_fds(&received_fds);
            if (error) *error = "malformed SCM_RIGHTS fd count";
            return BusReceiveStatus::Error;
        }
        const int* passed = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        received_fds.insert(received_fds.end(), passed, passed + count);
    }

    if ((raw.msg_flags & MSG_CTRUNC) != 0) {
        close_fds(&received_fds);
        if (error) *error = "SCM_RIGHTS control data was truncated";
        return BusReceiveStatus::Error;
    }
    if ((raw.msg_flags & MSG_TRUNC) != 0 || received < static_cast<ssize_t>(wire_header.size())) {
        close_fds(&received_fds);
        if (error) *error = "BUS packet is truncated";
        return BusReceiveStatus::Error;
    }

    std::string header_error;
    if (!decode_message_header(wire_header.data(), wire_header.size(), &message->header,
                               &header_error)) {
        close_fds(&received_fds);
        if (error) *error = header_error;
        return BusReceiveStatus::Error;
    }
    const size_t actual_payload = static_cast<size_t>(received) - wire_header.size();
    if (actual_payload != message->header.payload_size) {
        close_fds(&received_fds);
        if (error) *error = "BUS payload length does not match its header";
        return BusReceiveStatus::Error;
    }
    if (received_fds.size() != message->header.fd_count) {
        close_fds(&received_fds);
        if (error) *error = "BUS fd count does not match its header";
        return BusReceiveStatus::Error;
    }

    payload.resize(actual_payload);
    message->payload = std::move(payload);
    message->fds = std::move(received_fds);
    return BusReceiveStatus::Message;
}

BusWaitStatus BusConnection::wait_readable(int timeout_ms, std::string* error) const {
    if (fd_ < 0) {
        if (error) *error = "invalid BUS connection";
        return BusWaitStatus::Error;
    }
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int result = poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
        if (error) *error = "BUS receive timed out";
        return BusWaitStatus::Timeout;
    }
    if (result < 0) {
        if (errno == EINTR) return BusWaitStatus::Timeout;
        set_error_errno(error, "poll");
        return BusWaitStatus::Error;
    }
    if (descriptor.revents & POLLNVAL) {
        if (error) *error = "BUS fd is invalid";
        return BusWaitStatus::Error;
    }
    return BusWaitStatus::Ready;
}

bool resolve_unix_socket_path(const std::string& name, std::string* path,
                              std::string* error) {
    if (!path || name.empty()) {
        if (error) *error = "BUS socket name is empty";
        return false;
    }
    if (name.front() == '/') {
        *path = name;
    } else {
        if (name.find('/') != std::string::npos) {
            if (error) *error = "relative BUS socket names must not contain '/'";
            return false;
        }
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime_dir || runtime_dir[0] == '\0') {
            if (error) *error = "XDG_RUNTIME_DIR is not set for the BUS socket";
            return false;
        }
        *path = std::string(runtime_dir) + "/" + name;
    }
    return valid_socket_name(*path, error);
}

bool create_unix_seqpacket_listener(const std::string& name, int* fd,
                                    std::string* path, std::string* error) {
    if (!fd || !path || !resolve_unix_socket_path(name, path, error)) return false;
    *fd = -1;

    struct stat existing{};
    if (lstat(path->c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            if (error) *error = "BUS socket path already exists and is not a socket";
            return false;
        }
        if (unlink(path->c_str()) < 0) {
            set_error_errno(error, "unlink stale BUS socket");
            return false;
        }
    } else if (errno != ENOENT) {
        set_error_errno(error, "lstat BUS socket");
        return false;
    }

    const int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        set_error_errno(error, "socket(AF_UNIX, SOCK_SEQPACKET)");
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path->c_str(), path->size() + 1);
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path->size() + 1);
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), address_size) < 0) {
        set_error_errno(error, "bind BUS socket");
        ::close(socket_fd);
        return false;
    }
    if (chmod(path->c_str(), S_IRUSR | S_IWUSR) < 0) {
        set_error_errno(error, "chmod BUS socket");
        ::close(socket_fd);
        unlink(path->c_str());
        return false;
    }
    if (listen(socket_fd, 16) < 0) {
        set_error_errno(error, "listen BUS socket");
        ::close(socket_fd);
        unlink(path->c_str());
        return false;
    }
    if (!set_cloexec(socket_fd, error)) {
        ::close(socket_fd);
        unlink(path->c_str());
        return false;
    }
    *fd = socket_fd;
    return true;
}

bool connect_unix_seqpacket(const std::string& name, int* fd, std::string* error) {
    if (!fd) {
        if (error) *error = "BUS output fd is null";
        return false;
    }
    *fd = -1;
    std::string path;
    if (!resolve_unix_socket_path(name, &path, error)) return false;
    const int socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) {
        set_error_errno(error, "socket(AF_UNIX, SOCK_SEQPACKET)");
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), address_size) < 0) {
        set_error_errno(error, "connect BUS socket");
        ::close(socket_fd);
        return false;
    }
    if (!set_cloexec(socket_fd, error)) {
        ::close(socket_fd);
        return false;
    }
    *fd = socket_fd;
    return true;
}

}  // namespace kopms
