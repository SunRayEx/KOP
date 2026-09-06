#include "kopms_client.h"

#include <chrono>
#include <utility>

#include "frame_bridge.h"

namespace kopms {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

bool decode_server_error(const BusMessage& message, std::string* error) {
    KopmsErrorCode code = KOPMS_ERROR_MALFORMED;
    uint16_t type = 0;
    std::string text;
    std::string parse_error;
    if (!decode_error(message.payload, &code, &type, &text, &parse_error)) {
        set_error(error, "KOPMS-S sent an invalid ERROR: " + parse_error);
        return false;
    }
    set_error(error, "KOPMS-S error " + std::to_string(static_cast<uint32_t>(code)) +
                          " for message " + std::to_string(type) + ": " + text);
    return false;
}

}  // namespace

KopmsClient::~KopmsClient() { disconnect(); }

bool KopmsClient::connect(const std::string& socket_name, std::string* error) {
    disconnect();
    int fd = -1;
    if (!connect_unix_seqpacket(socket_name, &fd, error)) return false;
    connection_.reset(fd);
    next_client_sequence_ = 1;
    last_server_sequence_ = 0;
    next_frame_id_ = 1;
    last_released_frame_id_ = 0;
    last_release_status_ = KOPMS_FRAME_RELEASE_OK;
    return true;
}

bool KopmsClient::hello(uint64_t capabilities, std::string* error) {
    if (!connection_.valid() || hello_complete_) {
        set_error(error, "KOPMS-C transport is not ready for HELLO");
        return false;
    }
    KopmsHelloPayload hello_payload{};
    hello_payload.struct_size = KOPMS_HELLO_PAYLOAD_SIZE;
    hello_payload.protocol_major = KOPMS_PROTOCOL_MAJOR;
    hello_payload.protocol_minor = KOPMS_PROTOCOL_MINOR;
    hello_payload.capabilities = capabilities;
    hello_payload.max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    hello_payload.max_fds = KOPMS_PROTOCOL_MAX_FDS;
    std::string send_error;
    if (!connection_.send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                                  KOPMS_MESSAGE_HELLO, KOPMS_MESSAGE_FLAG_CONTROL,
                                  next_sequence(), encode_hello(hello_payload), {},
                                  &send_error)) {
        set_error(error, send_error);
        return false;
    }

    BusMessage message;
    if (!receive_one(-1, &message, error)) return false;
    if (message.header.type == KOPMS_MESSAGE_ERROR) {
        return decode_server_error(message, error);
    }
    if (message.header.type != KOPMS_MESSAGE_HELLO_ACK ||
        message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                  KOPMS_MESSAGE_FLAG_CONTROL) ||
        !message.fds.empty()) {
        set_error(error, "KOPMS-S did not answer HELLO with HELLO_ACK");
        return false;
    }
    if (message.header.sequence == 0 ||
        message.header.sequence <= last_server_sequence_) {
        set_error(error, "KOPMS-S HELLO_ACK sequence is invalid");
        return false;
    }
    last_server_sequence_ = message.header.sequence;
    if (message.header.major != KOPMS_PROTOCOL_MAJOR) {
        set_error(error, "KOPMS-S HELLO_ACK major version is invalid");
        return false;
    }

    KopmsHelloAckPayload ack{};
    std::string decode_error;
    if (!decode_hello_ack(message.payload, &ack, &decode_error)) {
        set_error(error, "invalid KOPMS-S HELLO_ACK: " + decode_error);
        return false;
    }
    if (ack.status != 0 || ack.selected_major != KOPMS_PROTOCOL_MAJOR ||
        ack.selected_minor > KOPMS_PROTOCOL_MINOR ||
        ack.max_payload < KOPMS_HELLO_ACK_PAYLOAD_SIZE ||
        ack.max_payload > KOPMS_PROTOCOL_MAX_PAYLOAD ||
        ack.max_fds > KOPMS_PROTOCOL_MAX_FDS) {
        set_error(error, "KOPMS-S HELLO_ACK negotiation is invalid");
        return false;
    }
    major_ = static_cast<uint16_t>(ack.selected_major);
    minor_ = static_cast<uint16_t>(ack.selected_minor);
    capabilities_ = ack.capabilities;
    max_payload_ = ack.max_payload;
    max_fds_ = ack.max_fds;
    hello_complete_ = true;
    return true;
}

bool KopmsClient::submit(KopmsFrameDescriptor* frame, uint32_t* frame_id,
                         std::string* error) {
    if (frame_id) *frame_id = 0;
    if (!connected()) {
        set_error(error, "KOPMS-C is not connected and negotiated");
        return false;
    }
    if ((capabilities_ & (KOPMS_PROTOCOL_CAP_DMABUF |
                          KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) !=
        (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) {
        set_error(error, "KOPMS-S did not negotiate DMA-BUF release capability");
        return false;
    }
    const uint32_t id = next_frame_id();
    KopmsFrameSubmitPayload frame_payload{};
    std::vector<int> fds;
    std::string make_error;
    if (!make_frame_submit(frame, id, &frame_payload, &fds, &make_error)) {
        set_error(error, make_error);
        return false;
    }
    const std::vector<uint8_t> payload = encode_frame_submit(frame_payload);
    if (payload.size() > max_payload_ || fds.size() > max_fds_) {
        set_error(error, "frame exceeds the negotiated KOPMS-S limits");
        return false;
    }

    frame->retain(frame);
    std::string send_error;
    if (!connection_.send_message(major_, minor_, KOPMS_MESSAGE_FRAME_SUBMIT,
                                  KOPMS_MESSAGE_FLAG_MEDIA, next_sequence(), payload,
                                  fds, &send_error)) {
        frame->release(frame);
        set_error(error, send_error);
        return false;
    }
    pending_frames_.emplace(id, frame);
    if (frame_id) *frame_id = id;
    return true;
}

bool KopmsClient::send_control(const KopmsControlCommandPayload& command,
                               const std::vector<uint8_t>& data,
                               uint64_t* request_sequence, std::string* error) {
    if (request_sequence) *request_sequence = 0;
    if (!connected()) {
        set_error(error, "KOPMS-C is not connected and negotiated");
        return false;
    }
    if ((capabilities_ & KOPMS_PROTOCOL_CAP_CONTROL_STATE) == 0) {
        set_error(error, "KOPMS-S did not negotiate control state capability");
        return false;
    }
    if (data.size() > KOPMS_CONTROL_DATA_MAX) {
        set_error(error, "KOPMS control data exceeds the limit");
        return false;
    }
    const std::vector<uint8_t> payload = encode_control_command(command, data);
    if (payload.size() > max_payload_) {
        set_error(error, "KOPMS control request exceeds the negotiated limit");
        return false;
    }
    const uint64_t sequence = next_sequence();
    std::string send_error;
    if (!connection_.send_message(major_, minor_, KOPMS_MESSAGE_CONTROL_COMMAND,
                                  KOPMS_MESSAGE_FLAG_CONTROL, sequence, payload, {},
                                  &send_error)) {
        set_error(error, send_error);
        return false;
    }
    pending_control_requests_.insert(sequence);
    if (request_sequence) *request_sequence = sequence;
    return true;
}

bool KopmsClient::wait_for_control_ack(uint64_t request_sequence, int timeout_ms,
                                       KopmsControlAckPayload* ack,
                                       std::string* error) {
    if (!ack) {
        set_error(error, "KOPMS control ACK target is null");
        return false;
    }
    if (!connected()) {
        set_error(error, "KOPMS-C is not connected and negotiated");
        return false;
    }
    const auto ready = control_acks_.find(request_sequence);
    if (ready != control_acks_.end()) {
        *ack = ready->second;
        control_acks_.erase(ready);
        return true;
    }
    if (pending_control_requests_.find(request_sequence) ==
        pending_control_requests_.end()) {
        set_error(error, "unknown KOPMS control request sequence");
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int remaining = timeout_ms;
        if (timeout_ms >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            const auto left = std::chrono::milliseconds(timeout_ms) - elapsed;
            remaining = left.count() <= 0 ? 0 : static_cast<int>(left.count());
        }
        if (!dispatch(remaining, error)) return false;
        const auto received = control_acks_.find(request_sequence);
        if (received != control_acks_.end()) {
            *ack = received->second;
            control_acks_.erase(received);
            return true;
        }
    }
}

bool KopmsClient::dispatch(int timeout_ms, std::string* error) {
    if (!connected()) {
        set_error(error, "KOPMS-C is not connected and negotiated");
        return false;
    }
    BusMessage message;
    if (!receive_one(timeout_ms, &message, error)) return false;
    return process_message(std::move(message), error);
}

bool KopmsClient::wait_for_release(uint32_t frame_id, int timeout_ms,
                                   std::string* error) {
    if (!connected()) {
        set_error(error, "KOPMS-C is not connected and negotiated");
        return false;
    }
    if (pending_frames_.find(frame_id) == pending_frames_.end()) {
        return last_released_frame_id_ == frame_id &&
               last_release_status_ == KOPMS_FRAME_RELEASE_OK;
    }

    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int remaining = timeout_ms;
        if (timeout_ms >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            const auto left = std::chrono::milliseconds(timeout_ms) - elapsed;
            remaining = left.count() <= 0 ? 0 : static_cast<int>(left.count());
        }
        if (!dispatch(remaining, error)) return false;
        if (pending_frames_.find(frame_id) == pending_frames_.end()) {
            return last_released_frame_id_ == frame_id &&
                   last_release_status_ == KOPMS_FRAME_RELEASE_OK;
        }
    }
}

void KopmsClient::disconnect() {
    if (connection_.valid() && hello_complete_) {
        KopmsGoodbyePayload goodbye{};
        goodbye.struct_size = KOPMS_GOODBYE_PAYLOAD_SIZE;
        goodbye.reason = 0;
        std::string ignored;
        connection_.send_message(major_, minor_, KOPMS_MESSAGE_GOODBYE,
                                  KOPMS_MESSAGE_FLAG_CONTROL, next_sequence(),
                                  encode_goodbye(goodbye), {}, &ignored);
    }
    release_pending();
    connection_.close();
    hello_complete_ = false;
    capabilities_ = 0;
    major_ = KOPMS_PROTOCOL_MAJOR;
    minor_ = KOPMS_PROTOCOL_MINOR;
    max_payload_ = KOPMS_PROTOCOL_MAX_PAYLOAD;
    max_fds_ = KOPMS_PROTOCOL_MAX_FDS;
    pending_control_requests_.clear();
    control_acks_.clear();
}

bool KopmsClient::process_message(BusMessage&& message, std::string* error) {
    if (message.header.sequence == 0 ||
        message.header.sequence <= last_server_sequence_) {
        set_error(error, "KOPMS-S message sequence is not strictly increasing");
        return false;
    }
    last_server_sequence_ = message.header.sequence;
    if (message.header.major != major_ || message.header.minor != minor_) {
        set_error(error, "KOPMS-S message version changed after HELLO");
        return false;
    }
    if (message.header.payload_size > max_payload_ || message.header.fd_count > max_fds_) {
        set_error(error, "KOPMS-S message exceeds negotiated limits");
        return false;
    }

    switch (message.header.type) {
        case KOPMS_MESSAGE_FRAME_RELEASE: {
            if (message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                         KOPMS_MESSAGE_FLAG_MEDIA)) {
                set_error(error, "FRAME_RELEASE has invalid lane flags");
                return false;
            }
            if (!message.fds.empty()) {
                set_error(error, "FRAME_RELEASE must not carry FDs");
                return false;
            }
            KopmsFrameReleasePayload release{};
            std::string decode_error;
            if (!decode_frame_release(message.payload, &release, &decode_error) ||
                release.frame_id == 0) {
                set_error(error, "invalid FRAME_RELEASE: " + decode_error);
                return false;
            }
            const auto it = pending_frames_.find(release.frame_id);
            if (it == pending_frames_.end()) {
                set_error(error, "FRAME_RELEASE refers to an unknown frame");
                return false;
            }
            KopmsFrameDescriptor* frame = it->second;
            pending_frames_.erase(it);
            last_released_frame_id_ = release.frame_id;
            last_release_status_ = release.status;
            if (getenv("KOPMS_RELEASE_DEBUG")) {
                std::fprintf(stderr, "[kopms-client] FRAME_RELEASE frame=%u status=%u pending=%zu\n",
                             release.frame_id, release.status, pending_frames_.size());
            }
            frame->release(frame);
            return true;
        }
        case KOPMS_MESSAGE_CONTROL_ACK: {
            if (message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                         KOPMS_MESSAGE_FLAG_CONTROL) ||
                !message.fds.empty()) {
                set_error(error, "CONTROL_ACK has invalid lane flags or FDs");
                return false;
            }
            KopmsControlAckPayload ack{};
            std::string decode_error;
            if (!decode_control_ack(message.payload, &ack, &decode_error) ||
                ack.request_sequence == 0) {
                set_error(error, "invalid CONTROL_ACK: " + decode_error);
                return false;
            }
            if (pending_control_requests_.erase(ack.request_sequence) == 0) {
                set_error(error, "CONTROL_ACK refers to an unknown request");
                return false;
            }
            control_acks_[ack.request_sequence] = ack;
            return true;
        }
        case KOPMS_MESSAGE_PONG:
            if (message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                         KOPMS_MESSAGE_FLAG_CONTROL) ||
                !message.payload.empty() || !message.fds.empty()) {
                set_error(error, "PONG must not carry a payload or FDs");
                return false;
            }
            return true;
        case KOPMS_MESSAGE_ERROR:
            return decode_server_error(message, error);
        default:
            set_error(error, "unexpected KOPMS-S message type");
            return false;
    }
}

bool KopmsClient::receive_one(int timeout_ms, BusMessage* message, std::string* error) {
    const BusWaitStatus wait_status = connection_.wait_readable(timeout_ms, error);
    if (wait_status != BusWaitStatus::Ready) return false;
    const BusReceiveStatus receive_status = connection_.receive(message, error);
    if (receive_status == BusReceiveStatus::Message) return true;
    if (receive_status == BusReceiveStatus::Closed) {
        set_error(error, "KOPMS-S closed the BUS connection");
    } else if (receive_status == BusReceiveStatus::WouldBlock) {
        set_error(error, "KOPMS-S packet was not readable");
    }
    return false;
}

uint64_t KopmsClient::next_sequence() {
    const uint64_t value = next_client_sequence_++;
    if (next_client_sequence_ == 0) next_client_sequence_ = 1;
    return value == 0 ? next_sequence() : value;
}

uint32_t KopmsClient::next_frame_id() {
    for (;;) {
        const uint32_t value = next_frame_id_++;
        if (next_frame_id_ == 0) next_frame_id_ = 1;
        if (value != 0 && pending_frames_.find(value) == pending_frames_.end()) return value;
    }
}

void KopmsClient::release_pending() {
    for (const auto& item : pending_frames_) {
        if (item.second && item.second->release) item.second->release(item.second);
    }
    pending_frames_.clear();
}

}  // namespace kopms
