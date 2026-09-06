#include "kopms_server.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "kop/log.h"
#include "wayland-server-core.h"

namespace kopms {

namespace {

constexpr const char* kTag = "kopms-bus";
constexpr uint64_t kServerCapabilities = KOPMS_PROTOCOL_CAP_DMABUF |
                                         KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                                         KOPMS_PROTOCOL_CAP_CONTROL_STATE |
                                         KOPMS_PROTOCOL_CAP_WAYLAND_BRIDGE |
                                         KOPMS_PROTOCOL_CAP_HANDLE_FRAMES |
                                         KOPMS_PROTOCOL_CAP_MODIFIERS |
                                         KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC;

void close_fds(std::vector<int>* fds) {
    if (!fds) return;
    for (int fd : *fds) {
        if (fd >= 0) ::close(fd);
    }
    fds->clear();
}

uint64_t next_sequence(uint64_t* sequence) {
    const uint64_t current = *sequence;
    ++*sequence;
    if (*sequence == 0) *sequence = 1;
    return current == 0 ? next_sequence(sequence) : current;
}

}  // namespace

KopmsReceivedFrame::~KopmsReceivedFrame() { close_fds(&fds); }

KopmsReceivedFrame::KopmsReceivedFrame(KopmsReceivedFrame&& other) noexcept
    : session_id(other.session_id), dma_buf_handle(other.dma_buf_handle),
      gpu_mode(other.gpu_mode), payload(other.payload), fds(std::move(other.fds)) {
    other.session_id = 0;
    other.dma_buf_handle = 0;
}

KopmsReceivedFrame& KopmsReceivedFrame::operator=(KopmsReceivedFrame&& other) noexcept {
    if (this == &other) return *this;
    close_fds(&fds);
    session_id = other.session_id;
    dma_buf_handle = other.dma_buf_handle;
    gpu_mode = other.gpu_mode;
    payload = other.payload;
    fds = std::move(other.fds);
    other.session_id = 0;
    other.dma_buf_handle = 0;
    return *this;
}

struct KopmsServer::Session {
    struct PendingFrame {
        uint64_t submit_sequence = 0;
        std::vector<int> fds;
    };

    Session(KopmsServer* owner, uint64_t id, int fd)
        : owner(owner), id(id), connection(fd) {}

    ~Session() {
        for (auto& item : pending_frames) close_fds(&item.second.fds);
        pending_frames.clear();
    }

    KopmsServer* owner = nullptr;
    uint64_t id = 0;
    BusConnection connection;
    wl_event_source* source = nullptr;
    bool hello_complete = false;
    uint16_t major = KOPMS_PROTOCOL_MAJOR;
    uint16_t minor = KOPMS_PROTOCOL_MINOR;
    uint64_t capabilities = 0;
    uint32_t max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    uint32_t max_fds = KOPMS_PROTOCOL_MAX_FDS;
    uint64_t last_client_sequence = 0;
    std::unordered_map<uint32_t, PendingFrame> pending_frames;
};

KopmsServer::KopmsServer(wl_event_loop* loop, std::string socket_name,
                         FrameHandler frame_handler, GpuMode gpu_mode)
    : loop_(loop), socket_name_(std::move(socket_name)),
      frame_handler_(std::move(frame_handler)), gpu_mode_(gpu_mode) {}

KopmsServer::~KopmsServer() { stop(); }

bool KopmsServer::start(std::string* error) {
    if (running()) {
        if (error) *error = "KOPMS-S is already running";
        return false;
    }
    if (!loop_) {
        if (error) *error = "KOPMS-S requires a Wayland event loop";
        return false;
    }
    if (!create_unix_seqpacket_listener(socket_name_, &listener_fd_, &socket_path_, error)) {
        return false;
    }
    const int old_flags = fcntl(listener_fd_, F_GETFL);
    if (old_flags < 0 || fcntl(listener_fd_, F_SETFL, old_flags | O_NONBLOCK) < 0) {
        if (error) *error = std::string("make KOPMS-S listener nonblocking: ") +
                            std::strerror(errno);
        ::close(listener_fd_);
        listener_fd_ = -1;
        unlink(socket_path_.c_str());
        socket_path_.clear();
        return false;
    }
    listener_source_ = wl_event_loop_add_fd(loop_, listener_fd_, WL_EVENT_READABLE,
                                             &KopmsServer::listener_event, this);
    if (!listener_source_) {
        if (error) *error = "cannot register KOPMS-S listener in Wayland event loop";
        ::close(listener_fd_);
        listener_fd_ = -1;
        unlink(socket_path_.c_str());
        socket_path_.clear();
        return false;
    }
    KOP_LOG_INFO(kTag, "KOPMS-S BUS2LAYER listening on %s", socket_path_.c_str());
    return true;
}

void KopmsServer::stop() {
    if (listener_source_) {
        wl_event_source_remove(listener_source_);
        listener_source_ = nullptr;
    }
    for (const auto& session : sessions_) {
        if (session->source) wl_event_source_remove(session->source);
        control_state_.remove_session(session->id);
    }
    sessions_.clear();
    if (listener_fd_ >= 0) {
        ::close(listener_fd_);
        listener_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
        socket_path_.clear();
    }
}

int KopmsServer::listener_event(int, uint32_t mask, void* data) {
    auto* server = static_cast<KopmsServer*>(data);
    if (!server) return 0;
    if (mask & WL_EVENT_ERROR) {
        KOP_LOG_ERROR(kTag, "KOPMS-S listener reported an event-loop error");
        return 0;
    }
    return server->accept_clients();
}

int KopmsServer::session_event(int, uint32_t mask, void* data) {
    auto* session = static_cast<Session*>(data);
    if (!session || !session->owner) return 0;
    return session->owner->process_session(session, mask);
}

int KopmsServer::accept_clients() {
    for (;;) {
        sockaddr_storage address{};
        socklen_t address_size = sizeof(address);
        const int client_fd = accept(listener_fd_, reinterpret_cast<sockaddr*>(&address),
                                     &address_size);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            KOP_LOG_WARN(kTag, "accept KOPMS-C failed: %s", std::strerror(errno));
            return 0;
        }

        const int fd_flags = fcntl(client_fd, F_GETFD);
        const int status_flags = fcntl(client_fd, F_GETFL);
        if (fd_flags < 0 || status_flags < 0 ||
            fcntl(client_fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0 ||
            fcntl(client_fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
            KOP_LOG_WARN(kTag, "configure KOPMS-C socket failed: %s", std::strerror(errno));
            ::close(client_fd);
            continue;
        }

        auto session = std::make_unique<Session>(this, next_session_id_++, client_fd);
        session->source = wl_event_loop_add_fd(loop_, client_fd, WL_EVENT_READABLE,
                                               &KopmsServer::session_event,
                                               session.get());
        if (!session->source) {
            KOP_LOG_WARN(kTag, "register KOPMS-C socket in event loop failed");
            ::close(client_fd);
            continue;
        }
        KOP_LOG_INFO(kTag, "KOPMS-C connected session=%llu",
                     static_cast<unsigned long long>(session->id));
        sessions_.push_back(std::move(session));
    }
}

int KopmsServer::process_session(Session* session, uint32_t mask) {
    if (!session) return 0;
    if (mask & WL_EVENT_ERROR) {
        KOP_LOG_WARN(kTag, "KOPMS-C session=%llu transport error",
                     static_cast<unsigned long long>(session->id));
        remove_session(session);
        return 0;
    }
    if (!(mask & WL_EVENT_READABLE)) {
        remove_session(session);
        return 0;
    }

    for (unsigned int i = 0; i < 64; ++i) {
        BusMessage message;
        std::string error;
        const BusReceiveStatus status = session->connection.receive(&message, &error);
        if (status == BusReceiveStatus::WouldBlock) return 0;
        if (status == BusReceiveStatus::Closed) {
            KOP_LOG_INFO(kTag, "KOPMS-C session=%llu closed normally",
                         static_cast<unsigned long long>(session->id));
            remove_session(session);
            return 0;
        }
        if (status == BusReceiveStatus::Error) {
            KOP_LOG_WARN(kTag, "KOPMS-C session=%llu malformed packet: %s",
                         static_cast<unsigned long long>(session->id), error.c_str());
            remove_session(session);
            return 0;
        }
        if (!handle_message(session, std::move(message))) {
            remove_session(session);
            return 0;
        }
    }
    return 0;
}

bool KopmsServer::handle_message(Session* session, BusMessage&& message) {
    if (!session) return false;
    const KopmsMessageHeader& header = message.header;
    if (header.sequence == 0 || header.sequence <= session->last_client_sequence) {
        send_error(session, KOPMS_ERROR_SEQUENCE, header.type,
                   "client message sequence is not strictly increasing");
        return false;
    }
    session->last_client_sequence = header.sequence;

    if (!session->hello_complete) {
        if (header.type != KOPMS_MESSAGE_HELLO ||
            header.flags != KOPMS_MESSAGE_FLAG_CONTROL || header.fd_count != 0) {
            send_error(session, KOPMS_ERROR_VERSION, header.type,
                       "HELLO must be the first packet and must not carry FDs");
            return false;
        }
        if (header.major != KOPMS_PROTOCOL_MAJOR) {
            send_error(session, KOPMS_ERROR_VERSION, header.type,
                       "KOPMS protocol major version is not supported");
            return false;
        }
        KopmsHelloPayload hello{};
        std::string error;
        if (!decode_hello(message.payload, &hello, &error)) {
            send_error(session, KOPMS_ERROR_MALFORMED, header.type, error);
            return false;
        }
        if (hello.protocol_major != KOPMS_PROTOCOL_MAJOR) {
            send_error(session, KOPMS_ERROR_VERSION, header.type,
                       "HELLO payload major version is not supported");
            return false;
        }
        if (hello.max_payload != 0 && hello.max_payload < KOPMS_HELLO_ACK_PAYLOAD_SIZE) {
            send_error(session, KOPMS_ERROR_LIMIT, header.type,
                       "client payload limit is smaller than HELLO_ACK");
            return false;
        }
        session->minor = static_cast<uint16_t>(
            std::min<uint32_t>(hello.protocol_minor, KOPMS_PROTOCOL_MINOR));
        session->max_payload = hello.max_payload == 0
                                   ? KOPMS_PROTOCOL_MAX_PAYLOAD
                                   : std::min<uint32_t>(hello.max_payload,
                                                        KOPMS_PROTOCOL_MAX_PAYLOAD);
        session->max_fds = hello.max_fds == 0
                               ? KOPMS_PROTOCOL_MAX_FDS
                               : std::min<uint32_t>(hello.max_fds, KOPMS_PROTOCOL_MAX_FDS);

        KopmsHelloAckPayload ack{};
        ack.struct_size = KOPMS_HELLO_ACK_PAYLOAD_SIZE;
        ack.status = 0;
        ack.selected_major = KOPMS_PROTOCOL_MAJOR;
        ack.selected_minor = session->minor;
        ack.capabilities = kServerCapabilities & hello.capabilities;
        ack.max_payload = session->max_payload;
        ack.max_fds = session->max_fds;
        if (!session->connection.send_message(
                KOPMS_PROTOCOL_MAJOR, session->minor, KOPMS_MESSAGE_HELLO_ACK,
                KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                next_sequence(&next_server_sequence_),
                encode_hello_ack(ack), {}, &error)) {
            KOP_LOG_WARN(kTag, "KOPMS-C session=%llu HELLO_ACK failed: %s",
                         static_cast<unsigned long long>(session->id), error.c_str());
            return false;
        }
        session->hello_complete = true;
        session->capabilities = ack.capabilities;
        control_state_.register_session(session->id);
        KOP_LOG_INFO(kTag, "KOPMS-C session=%llu handshake complete minor=%u caps=0x%llx",
                     static_cast<unsigned long long>(session->id), session->minor,
                     static_cast<unsigned long long>(ack.capabilities));
        return true;
    }

    if (header.major != session->major || header.minor != session->minor) {
        send_error(session, KOPMS_ERROR_VERSION, header.type,
                   "message version does not match the negotiated session");
        return false;
    }
    if (header.payload_size > session->max_payload || header.fd_count > session->max_fds) {
        send_error(session, KOPMS_ERROR_LIMIT, header.type,
                   "message exceeds the negotiated session limits");
        return false;
    }

    if (header.type == KOPMS_MESSAGE_FRAME_SUBMIT) {
        if ((header.flags & KOPMS_MESSAGE_FLAG_MEDIA) == 0 ||
            (header.flags & KOPMS_MESSAGE_FLAG_CONTROL) != 0) {
            send_error(session, KOPMS_ERROR_MALFORMED, header.type,
                       "FRAME_SUBMIT must use the MEDIA lane");
            return false;
        }
    } else if (header.type == KOPMS_MESSAGE_CONTROL_COMMAND ||
               header.type == KOPMS_MESSAGE_PING ||
               header.type == KOPMS_MESSAGE_GOODBYE) {
        if ((header.flags & KOPMS_MESSAGE_FLAG_CONTROL) == 0 ||
            (header.flags & KOPMS_MESSAGE_FLAG_MEDIA) != 0) {
            send_error(session, KOPMS_ERROR_MALFORMED, header.type,
                       "control messages must use the CONTROL lane");
            return false;
        }
    }

    switch (header.type) {
        case KOPMS_MESSAGE_FRAME_SUBMIT: {
            const uint64_t required_capabilities = KOPMS_PROTOCOL_CAP_DMABUF |
                                                   KOPMS_PROTOCOL_CAP_FRAME_RELEASE;
            if ((session->capabilities & required_capabilities) !=
                    required_capabilities ||
                message.fds.empty()) {
                send_error(session, KOPMS_ERROR_UNSUPPORTED, header.type,
                           "DMA-BUF frame submission is unavailable");
                return false;
            }
            KopmsFrameSubmitPayload frame{};
            std::string error;
            if (!decode_frame_submit(message.payload, &frame, &error) ||
                !validate_frame_submit_caps(frame, message.fds.size(),
                                            session->capabilities, &error)) {
                send_error(session, KOPMS_ERROR_FRAME, header.type, error);
                return false;
            }
            if (session->pending_frames.find(frame.frame_id) != session->pending_frames.end()) {
                send_error(session, KOPMS_ERROR_SEQUENCE, header.type,
                           "frame id is already in flight");
                return false;
            }

            KopmsReceivedFrame received;
            received.session_id = session->id;
            received.gpu_mode = gpu_mode_;
            received.payload = frame;
            received.fds = message.take_fds();
            // The wire payload carries indexes, never process-local numbers.
            // SCM_RIGHTS has already installed these descriptors in the
            // server, so this is the only handle materialization step.
            const int32_t primary_index = frame.planes[0].fd_index;
            received.dma_buf_handle =
                static_cast<uint64_t>(received.fds[primary_index]);
            const FrameDisposition disposition =
                frame_handler_ ? frame_handler_(received) : FrameDisposition::Release;
            if (disposition == FrameDisposition::Retain) {
                Session::PendingFrame pending;
                pending.submit_sequence = header.sequence;
                pending.fds = std::move(received.fds);
                session->pending_frames.emplace(frame.frame_id, std::move(pending));
                KOP_LOG_DEBUG(kTag, "KOPMS-C session=%llu retained frame=%u",
                              static_cast<unsigned long long>(session->id), frame.frame_id);
                return true;
            }
            if (disposition == FrameDisposition::ReleaseDropped) {
                if (getenv("KOPMS_RELEASE_DEBUG")) {
                    KOP_LOG_DEBUG(kTag, "handler 丢弃 frame=%u（DROPPED）",
                                  frame.frame_id);
                }
                if (!send_release(session, frame.frame_id,
                                  KOPMS_FRAME_RELEASE_DROPPED)) {
                    return false;
                }
                KOP_LOG_DEBUG(kTag, "KOPMS-C session=%llu dropped frame=%u (backpressure)",
                              static_cast<unsigned long long>(session->id),
                              frame.frame_id);
                return true;
            }
            if (!send_release(session, frame.frame_id, KOPMS_FRAME_RELEASE_OK)) return false;
            KOP_LOG_DEBUG(kTag, "KOPMS-C session=%llu released frame=%u",
                          static_cast<unsigned long long>(session->id), frame.frame_id);
            return true;
        }
        case KOPMS_MESSAGE_PING:
            if (!message.payload.empty() || !message.fds.empty()) {
                send_error(session, KOPMS_ERROR_MALFORMED, header.type,
                           "PING must not carry a payload or FDs");
                return false;
            }
            return session->connection.send_message(
                session->major, session->minor, KOPMS_MESSAGE_PONG,
                KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                next_sequence(&next_server_sequence_), {}, {}, nullptr);
        case KOPMS_MESSAGE_CONTROL_COMMAND: {
            if (!message.fds.empty() ||
                (session->capabilities & KOPMS_PROTOCOL_CAP_CONTROL_STATE) == 0) {
                send_error(session, KOPMS_ERROR_UNSUPPORTED, header.type,
                           "control state is unavailable");
                return false;
            }
            KopmsControlCommandPayload command{};
            std::vector<uint8_t> data;
            std::string error;
            if (!decode_control_command(message.payload, &command, &data, &error)) {
                send_error(session, KOPMS_ERROR_MALFORMED, header.type, error);
                return false;
            }
            KopmsControlAckPayload ack{};
            if (!control_state_.apply(session->id, command, data, &ack, &error)) {
                send_error(session, KOPMS_ERROR_MALFORMED, header.type, error);
                return false;
            }
            ack.request_sequence = header.sequence;
            return send_control_ack(session, ack);
        }
        case KOPMS_MESSAGE_GOODBYE: {
            KopmsGoodbyePayload goodbye{};
            std::string error;
            if (!decode_goodbye(message.payload, &goodbye, &error) ||
                !message.fds.empty()) {
                send_error(session, KOPMS_ERROR_MALFORMED, header.type,
                           error.empty() ? "GOODBYE payload is invalid" : error);
                return false;
            }
            KOP_LOG_INFO(kTag, "KOPMS-C session=%llu sent goodbye reason=%u",
                         static_cast<unsigned long long>(session->id), goodbye.reason);
            return false;
        }
        default:
            send_error(session, KOPMS_ERROR_UNSUPPORTED, header.type,
                       "message type is not supported by KOPMS-S");
            return false;
    }
}

bool KopmsServer::send_error(Session* session, KopmsErrorCode code, uint16_t type,
                             const std::string& message) {
    if (!session) return false;
    std::string ignored;
    const uint16_t lane = type == KOPMS_MESSAGE_FRAME_SUBMIT
                              ? KOPMS_MESSAGE_FLAG_MEDIA
                              : KOPMS_MESSAGE_FLAG_CONTROL;
    return session->connection.send_message(
        KOPMS_PROTOCOL_MAJOR, session->hello_complete ? session->minor : KOPMS_PROTOCOL_MINOR,
        KOPMS_MESSAGE_ERROR, KOPMS_MESSAGE_FLAG_REPLY | lane,
        next_sequence(&next_server_sequence_), encode_error(code, type, message), {},
        &ignored);
}

bool KopmsServer::send_release(Session* session, uint32_t frame_id, uint32_t status) {
    if (!session) return false;
    KopmsFrameReleasePayload release{};
    release.struct_size = KOPMS_FRAME_RELEASE_PAYLOAD_SIZE;
    release.frame_id = frame_id;
    release.status = status;
    std::string error;
    return session->connection.send_message(
        session->major, session->minor, KOPMS_MESSAGE_FRAME_RELEASE,
        KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_MEDIA,
        next_sequence(&next_server_sequence_),
        encode_frame_release(release), {}, &error);
}

bool KopmsServer::send_control_ack(Session* session,
                                   const KopmsControlAckPayload& ack) {
    if (!session) return false;
    std::string error;
    return session->connection.send_message(
        session->major, session->minor, KOPMS_MESSAGE_CONTROL_ACK,
        KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
        next_sequence(&next_server_sequence_), encode_control_ack(ack), {}, &error);
}

std::vector<ControlState::WindowInfo> KopmsServer::control_snapshot() const {
    return control_state_.snapshot();
}

uint64_t KopmsServer::focused_control_window() const {
    return control_state_.focused_window();
}

bool KopmsServer::release_frame(uint64_t session_id, uint32_t frame_id, uint32_t status) {    for (const auto& session : sessions_) {
        if (session->id != session_id) continue;
        const auto it = session->pending_frames.find(frame_id);
        if (it == session->pending_frames.end()) return false;
        const bool sent = send_release(session.get(), frame_id, status);
        close_fds(&it->second.fds);
        session->pending_frames.erase(it);
        if (!sent) {
            remove_session(session.get());
            return false;
        }
        return true;
    }
    return false;
}

void KopmsServer::remove_session(Session* session) {
    if (!session) return;
    if (session->source) {
        wl_event_source_remove(session->source);
        session->source = nullptr;
    }
    control_state_.remove_session(session->id);
    KOP_LOG_INFO(kTag, "KOPMS-C session=%llu disconnected",
                 static_cast<unsigned long long>(session->id));
    sessions_.remove_if([session](const std::unique_ptr<Session>& item) {
        return item.get() == session;
    });
}

}  // namespace kopms
