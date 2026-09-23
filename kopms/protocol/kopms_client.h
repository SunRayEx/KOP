// KOPMS-C client session API.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bus2layer.h"

namespace kopms {

class KopmsClient {
public:
    using FrameReleaseObserver = std::function<void(uint32_t frame_id, uint32_t status)>;
    KopmsClient() = default;
    ~KopmsClient();

    KopmsClient(const KopmsClient&) = delete;
    KopmsClient& operator=(const KopmsClient&) = delete;

    bool connect(const std::string& socket_name, std::string* error);
    bool hello(uint64_t capabilities, std::string* error);
    bool connected() const { return connection_.valid() && hello_complete_; }

    // submit() retains the descriptor until KOPMS-S sends FRAME_RELEASE. Once
    // connected, a rejected attempt also invokes release exactly once, so a
    // submission wrapper can keep one ownership path for success and failure.
    bool submit(KopmsFrameDescriptor* frame, uint32_t* frame_id, std::string* error);

    // Sends a control-plane request. The request remains pending until its
    // CONTROL_ACK is consumed by wait_for_control_ack() or dispatch().
    bool send_control(const KopmsControlCommandPayload& command,
                      const std::vector<uint8_t>& data,
                      uint64_t* request_sequence, std::string* error);
    bool wait_for_control_ack(uint64_t request_sequence, int timeout_ms,
                              KopmsControlAckPayload* ack, std::string* error);

    // Dispatch one server packet. A timeout is expressed in milliseconds;
    // -1 waits indefinitely.
    bool dispatch(int timeout_ms, std::string* error);
    bool wait_for_release(uint32_t frame_id, int timeout_ms, std::string* error);

    // Sends GOODBYE when possible and releases all local in-flight references.
    void disconnect();

    uint64_t capabilities() const { return capabilities_; }
    uint16_t negotiated_minor() const { return minor_; }
    void set_frame_release_observer(FrameReleaseObserver observer) {
        frame_release_observer_ = std::move(observer);
    }

private:
    bool process_message(BusMessage&& message, std::string* error);
    bool receive_one(int timeout_ms, BusMessage* message, std::string* error);
    uint64_t next_sequence();
    uint32_t next_frame_id();
    void release_pending();

    BusConnection connection_;
    bool hello_complete_ = false;
    uint16_t major_ = KOPMS_PROTOCOL_MAJOR;
    uint16_t minor_ = KOPMS_PROTOCOL_MINOR;
    uint64_t capabilities_ = 0;
    uint32_t max_payload_ = KOPMS_PROTOCOL_MAX_PAYLOAD;
    uint32_t max_fds_ = KOPMS_PROTOCOL_MAX_FDS;
    uint64_t next_client_sequence_ = 1;
    uint64_t last_server_sequence_ = 0;
    uint32_t next_frame_id_ = 1;
    uint32_t last_released_frame_id_ = 0;
    uint32_t last_release_status_ = KOPMS_FRAME_RELEASE_OK;
    FrameReleaseObserver frame_release_observer_;
    std::unordered_map<uint32_t, KopmsFrameDescriptor*> pending_frames_;
    std::unordered_set<uint64_t> pending_control_requests_;
    std::unordered_map<uint64_t, KopmsControlAckPayload> control_acks_;
};

}  // namespace kopms
