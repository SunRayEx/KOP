// BUS2LAYER transport for KOPMS-S/KOPMS-C.
//
// The BUS layer owns sockets, packet boundaries, ancillary file descriptors,
// and transport limits. Protocol objects are decoded by the layer above it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kopms_protocol.h"

namespace kopms {

enum class BusReceiveStatus {
    Message,
    WouldBlock,
    Closed,
    Error,
};

enum class BusWaitStatus {
    Ready,
    Timeout,
    Error,
};

class BusMessage {
public:
    BusMessage() = default;
    ~BusMessage();

    BusMessage(const BusMessage&) = delete;
    BusMessage& operator=(const BusMessage&) = delete;
    BusMessage(BusMessage&& other) noexcept;
    BusMessage& operator=(BusMessage&& other) noexcept;

    void clear();
    std::vector<int> take_fds();

    KopmsMessageHeader header{};
    std::vector<uint8_t> payload;
    std::vector<int> fds;
};

class BusConnection {
public:
    BusConnection() = default;
    explicit BusConnection(int fd, bool owns_fd = true) : fd_(fd), owns_fd_(owns_fd) {}
    ~BusConnection();

    BusConnection(const BusConnection&) = delete;
    BusConnection& operator=(const BusConnection&) = delete;
    BusConnection(BusConnection&& other) noexcept;
    BusConnection& operator=(BusConnection&& other) noexcept;

    void reset(int fd = -1, bool owns_fd = true);
    void close();
    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    bool set_nonblocking(bool enabled, std::string* error);
    bool send_message(uint16_t major, uint16_t minor, uint16_t type,
                      uint16_t flags, uint64_t sequence,
                      const std::vector<uint8_t>& payload,
                      const std::vector<int>& fds, std::string* error);
    BusReceiveStatus receive(BusMessage* message, std::string* error);
    BusWaitStatus wait_readable(int timeout_ms, std::string* error) const;

private:
    int fd_ = -1;
    bool owns_fd_ = true;
};

// Names without a leading slash are resolved below XDG_RUNTIME_DIR. Absolute
// paths are accepted for tests and for a separately managed runtime directory.
bool resolve_unix_socket_path(const std::string& name, std::string* path,
                              std::string* error);
bool create_unix_seqpacket_listener(const std::string& name, int* fd,
                                    std::string* path, std::string* error);
bool connect_unix_seqpacket(const std::string& name, int* fd, std::string* error);

}  // namespace kopms
