#include "kopnet/remote_session.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <unistd.h>
#include <cstring>
#include <utility>

#include "kop/log.h"
#include "kopnet/adapters.hpp"
#include "kopnet/resilient.hpp"
#include "lane_session.hpp"

namespace kopnet {

// 线编解码函数位于 kopms 命名空间；这里取短别名，避免与 kopnet 自身类型混淆。
namespace codec = ::kopms;

namespace {

constexpr const char* kTag = "kopnet-remote";

// 与 KOPMS-S 一致的服务端默认能力集：serve 侧未显式声明时回送对端。
constexpr uint64_t kDefaultServerCapabilities =
    KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
    KOPMS_PROTOCOL_CAP_CONTROL_STATE | KOPMS_PROTOCOL_CAP_WAYLAND_BRIDGE |
    KOPMS_PROTOCOL_CAP_HANDLE_FRAMES | KOPMS_PROTOCOL_CAP_MODIFIERS |
    KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC | KOPMS_PROTOCOL_CAP_COLOR_METADATA;

uint32_t release_status_of(FrameDisposition disposition) {
    switch (disposition) {
        case FrameDisposition::Release:
            return KOPMS_FRAME_RELEASE_OK;
        case FrameDisposition::ReleaseDropped:
            return KOPMS_FRAME_RELEASE_DROPPED;
        case FrameDisposition::ReleaseRejected:
            return KOPMS_FRAME_RELEASE_REJECTED;
        case FrameDisposition::Retain:
            return UINT32_MAX;  // 调用方负责稍后回送
    }
    return KOPMS_FRAME_RELEASE_REJECTED;
}

void close_owned_fds(const std::vector<int>& fds) {
    for (int fd : fds) {
        if (fd >= 0) ::close(fd);
    }
}

}  // namespace

RemoteFrame::~RemoteFrame() {
    close_fds();
}

RemoteFrame::RemoteFrame(RemoteFrame&& other) noexcept
    : payload(other.payload), fds(std::move(other.fds)) {
    std::memset(&other.payload, 0, sizeof(other.payload));
}

RemoteFrame& RemoteFrame::operator=(RemoteFrame&& other) noexcept {
    if (this != &other) {
        close_fds();
        payload = other.payload;
        fds = std::move(other.fds);
        std::memset(&other.payload, 0, sizeof(other.payload));
    }
    return *this;
}

void RemoteFrame::close_fds() {
    close_owned_fds(fds);
}

RemoteSession::RemoteSession() = default;

RemoteSession::~RemoteSession() {
    disconnect();
}

// ---- ResilientLaneSession：委托给 ResilientSession（逻辑通道号）----

uint32_t ResilientLaneSession::open_channel(const std::string& kind, ChannelMode mode,
                                            int timeout_ms, DataHandler handler,
                                            std::string* error) {
    return session_->open_channel(kind, mode, timeout_ms, handler, error);
}

void ResilientLaneSession::set_channel_handler(ChannelHandler handler) {
    session_->set_channel_handler(std::move(handler));
}

void ResilientLaneSession::set_data_handler(uint32_t channel, DataHandler handler) {
    session_->set_data_handler(channel, std::move(handler));
}

void ResilientLaneSession::close_channel(uint32_t channel) {
    session_->close_channel(channel);
}

void ResilientLaneSession::set_close_handler(CloseHandler handler) {
    session_->set_close_handler(std::move(handler));
}

void ResilientLaneSession::set_logger(Logger logger) {
    session_->set_logger(std::move(logger));
}

SendStatus ResilientLaneSession::send(uint32_t channel, const uint8_t* data, size_t len,
                                      const std::vector<int>* fds, int timeout_ms) {
    return session_->send(channel, data, len, fds, timeout_ms);
}

bool ResilientLaneSession::wait_connected(int timeout_ms) {
    return session_->wait_connected(timeout_ms);
}

bool ResilientLaneSession::connected() const { return session_->connected(); }

bool ResilientLaneSession::done() const { return session_->reconnect_done(); }

bool ResilientLaneSession::supports_fds() const { return session_->supports_fds(); }

void ResilientLaneSession::stop() { session_->stop(); }

bool RemoteSession::open(const std::string& uri, const Options& opts,
                         std::string* error) {
    if (lane_) {
        if (error) *error = "会话已打开";
        return false;
    }
    opts_ = opts;
    is_serving_ = false;
    const int open_ms = opts.hello_timeout_ms > 0 ? opts.hello_timeout_ms : 3000;

    if (opts.auto_reconnect) {
        ResilientSession::Options ropts;
        ropts.endpoints.push_back(uri);
        for (const auto& fb : opts.fallback_endpoints) ropts.endpoints.push_back(fb);
        ropts.base_delay_ms = opts.reconnect_base_delay_ms;
        ropts.max_delay_ms = opts.reconnect_max_delay_ms;
        ropts.connect_timeout_ms = open_ms;
        ropts.tunnel_opts.handshake_ms = open_ms;
        auto rs = std::make_unique<ResilientSession>(ropts);
        rs->set_logger([this](const std::string& message) { log(message); });
        rs->set_channel_handler(
            [this](uint32_t id, const std::string& kind, ChannelMode mode) {
                on_channel_open(id, kind, mode);
            });
        rs->set_state_handler([this](bool connected) {
            on_transport_state(connected);
        });
        if (!rs->start(error)) return false;
        lane_ = std::make_unique<ResilientLaneSession>(std::move(rs));
    } else {
        TunnelSession::Options tunnel_opts;
        tunnel_opts.handshake_ms = open_ms;
        auto session = tunnel_dial(uri, tunnel_opts, error);
        if (!session) return false;
        session->set_close_handler([this] { mark_done(); });
        session->set_logger([this](const std::string& message) { log(message); });
        lane_ = std::make_unique<PlainLaneSession>(std::move(session));
    }

    // handler 在 OPEN 发出前就装到通道上，对端首批数据到达时回调必然已就位。
    // 可重连路径下通道号是逻辑号，处理器随每次重连自动重装。
    control_channel_ = lane_->open_channel(
        kKopmsControlLane, ChannelMode::Stream, open_ms,
        [this](uint32_t id, const std::vector<uint8_t>& data,
               const std::vector<int>& fds) { on_control_data(id, data, fds); },
        error);
    if (!control_channel_) {
        lane_->stop();
        lane_.reset();
        return false;
    }
    media_channel_ = lane_->open_channel(
        kKopmsMediaLane, ChannelMode::Stream, open_ms,
        [this](uint32_t id, const std::vector<uint8_t>& data,
               const std::vector<int>& fds) { on_media_data(id, data, fds); },
        error);
    if (!media_channel_) {
        lane_->stop();
        lane_.reset();
        control_channel_ = 0;
        return false;
    }
    return true;
}

bool RemoteSession::serve(std::unique_ptr<TunnelSession> session, const Options& opts,
                          std::string* error) {
    if (!session) {
        if (error) *error = "隧道会话为空";
        return false;
    }
    if (session->alive()) {
        if (error) *error = "隧道会话必须尚未 start（回调在 start 前注册）";
        return false;
    }
    opts_ = opts;
    is_serving_ = true;
    lane_ = std::make_unique<PlainLaneSession>(std::move(session));
    lane_->set_close_handler([this] { mark_done(); });
    lane_->set_logger([this](const std::string& message) { log(message); });
    // 通道回调在 OPEN_ACK 与额度授予之前触发，此处装 data handler 必然早于
    // 该通道的首帧数据（见 TunnelSession 的时序保证）。
    lane_->set_channel_handler(
        [this](uint32_t id, const std::string& kind, ChannelMode mode) {
            on_channel_open(id, kind, mode);
        });
    if (!lane_->start(error)) {
        lane_.reset();
        return false;
    }
    return true;
}

bool RemoteSession::wait_done(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (done_.load()) return true;
    if (timeout_ms <= 0) return done_.load();
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return done_.load(); });
    return done_.load();
}

bool RemoteSession::hello(uint64_t capabilities, std::string* error) {
    if (!lane_ || is_serving_) {
        if (error) *error = "仅 Connect 角色可主动 HELLO";
        return false;
    }
    if (hello_complete_.load()) return true;
    // 可重连路径下首个连接可能仍在后台拨号：先等传输连通，否则 HELLO 无处可发
    const int wait_ms = opts_.hello_timeout_ms > 0 ? opts_.hello_timeout_ms : 3000;
    if (!lane_->wait_connected(wait_ms)) {
        if (error) *error = "等待首个连接建立超时";
        return false;
    }
    local_caps_ = capabilities;
    KopmsHelloPayload hello_payload{};
    hello_payload.struct_size = KOPMS_HELLO_PAYLOAD_SIZE;
    hello_payload.protocol_major = KOPMS_PROTOCOL_MAJOR;
    hello_payload.protocol_minor = KOPMS_PROTOCOL_MINOR;
    hello_payload.capabilities = capabilities;
    hello_payload.max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    hello_payload.max_fds = KOPMS_PROTOCOL_MAX_FDS;
    if (!send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_HELLO,
                      KOPMS_MESSAGE_FLAG_CONTROL, next_send_sequence(),
                      codec::encode_hello(hello_payload), {}, error)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = cv_.wait_for(
        lock, std::chrono::milliseconds(
                  opts_.hello_timeout_ms > 0 ? opts_.hello_timeout_ms : 3000),
        [this] { return hello_complete_.load() || done_.load() || !lane_->connected(); });
    if (!ok || !hello_complete_.load()) {
        if (error) *error = last_error_.empty() ? "KOPMS HELLO 协商超时" : last_error_;
        return false;
    }
    return true;
}

bool RemoteSession::submit(KopmsFrameDescriptor* frame, uint32_t* frame_id,
                           std::string* error) {
    if (frame_id) *frame_id = 0;
    if (!connected()) {
        if (error) *error = "会话未完成 KOPMS 协商";
        return false;
    }
    const auto release_frame_now = [frame] {
        if (frame && frame->release) frame->release(frame);
    };
    if ((negotiated_caps_ & (KOPMS_PROTOCOL_CAP_DMABUF |
                             KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) !=
        (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) {
        if (error) *error = "对端未协商 DMA-BUF 帧释放能力";
        release_frame_now();
        return false;
    }
    KopmsFrameSubmitPayload frame_payload{};
    std::vector<int> fds;
    std::string make_error;
    // 先登记在飞帧再发送：FRAME_RELEASE 可能在 send 返回前就到达，此时
    // pending_frames_ 必须已有条目，否则 release 观察者会错过这一次释放。
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_frame_id_++;
        pending_frames_.emplace(id, frame);
    }
    if (!codec::make_frame_submit(frame, id, &frame_payload, &fds, &make_error)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_frames_.erase(id);
        }
        if (error) *error = make_error;
        release_frame_now();
        return false;
    }
    if ((negotiated_caps_ & KOPMS_PROTOCOL_CAP_COLOR_METADATA) == 0) {
        // 与 1.1 对端保持线兼容：裁掉可选的色彩元数据尾部。
        frame_payload.color = {};
        frame_payload.struct_size = KOPMS_FRAME_SUBMIT_BASE_SIZE;
    }
    const std::vector<uint8_t> payload = codec::encode_frame_submit(frame_payload);
    if (payload.size() > KOPMS_PROTOCOL_MAX_PAYLOAD || fds.size() > max_fds_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_frames_.erase(id);
        }
        if (error) *error = "帧超过协商的 KOPMS 上限";
        close_owned_fds(fds);
        release_frame_now();
        return false;
    }
    // 一条 KOPMS 消息 = 32B 头 + payload，整条作为一次隧道发送（fd 只随
    // 这一次发送移交），故必须走 send_message 而非裸发 payload。
    if (!send_message(KOPMS_MESSAGE_FLAG_MEDIA, KOPMS_MESSAGE_FRAME_SUBMIT,
                      KOPMS_MESSAGE_FLAG_MEDIA, next_send_sequence(), payload, fds,
                      error)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_frames_.erase(id);
        }
        close_owned_fds(fds);  // 隧道只在发送成功时接管 fd
        release_frame_now();
        return false;
    }
    if (frame_id) *frame_id = id;
    return true;
}

bool RemoteSession::send_control(const KopmsControlCommandPayload& command,
                                 const std::vector<uint8_t>& data,
                                 uint64_t* request_sequence, std::string* error) {
    if (request_sequence) *request_sequence = 0;
    if (!connected()) {
        if (error) *error = "会话未完成 KOPMS 协商";
        return false;
    }
    if ((negotiated_caps_ & KOPMS_PROTOCOL_CAP_CONTROL_STATE) == 0) {
        if (error) *error = "对端未协商控制状态能力";
        return false;
    }
    if (data.size() > KOPMS_CONTROL_DATA_MAX) {
        if (error) *error = "控制数据超过上限";
        return false;
    }
    const std::vector<uint8_t> payload = codec::encode_control_command(command, data);
    if (payload.size() > KOPMS_PROTOCOL_MAX_PAYLOAD) {
        if (error) *error = "控制请求超过协商上限";
        return false;
    }
    const uint64_t sequence = next_send_sequence();
    if (!send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_CONTROL_COMMAND,
                      KOPMS_MESSAGE_FLAG_CONTROL, sequence, payload, {}, error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_control_sequences_.insert(sequence);
    }
    if (request_sequence) *request_sequence = sequence;
    return true;
}

bool RemoteSession::wait_for_control_ack(uint64_t request_sequence, int timeout_ms,
                                         KopmsControlAckPayload* ack,
                                         std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [this, request_sequence] {
            return control_acks_.count(request_sequence) > 0 || done_.load() ||
                   !lane_->connected();
        });
    if (!ok) {
        if (error) *error = "等待 CONTROL_ACK 超时";
        return false;
    }
    const auto it = control_acks_.find(request_sequence);
    if (it == control_acks_.end()) {
        if (error)
            *error = lane_->connected() ? "会话已结束" : "会话已断开，请求丢失";
        return false;
    }
    if (ack) *ack = it->second;
    control_acks_.erase(it);
    return true;
}

bool RemoteSession::wait_for_release(uint32_t frame_id, int timeout_ms,
                                     std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [this, frame_id] {
            return release_status_.count(frame_id) > 0 || done_.load() ||
                   !lane_->connected();
        });
    if (!ok) {
        if (error) *error = "等待 FRAME_RELEASE 超时";
        return false;
    }
    const auto it = release_status_.find(frame_id);
    if (it == release_status_.end()) {
        if (error)
            *error = lane_->connected() ? "会话已结束" : "会话已断开，帧丢失";
        return false;
    }
    const uint32_t status = it->second;
    release_status_.erase(it);
    if (status != KOPMS_FRAME_RELEASE_OK) {
        if (error) *error = std::string("帧被拒收/丢弃: status=") + std::to_string(status);
        return false;
    }
    return true;
}

bool RemoteSession::ping(std::string* error) {
    if (!connected()) {
        if (error) *error = "会话未完成 KOPMS 协商";
        return false;
    }
    return send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_PING,
                        KOPMS_MESSAGE_FLAG_CONTROL, next_send_sequence(), {}, {},
                        error);
}

bool RemoteSession::send_raw_control(const std::vector<uint8_t>& message,
                                     std::string* error) {
    if (!lane_ || control_channel_ == 0) {
        if (error) *error = "control lane 尚未建立";
        return false;
    }
    const SendStatus st = lane_->send(control_channel_, message.data(),
                                         message.size(), nullptr,
                                         opts_.send_timeout_ms);
    if (st != SendStatus::Ok) {
        if (error) *error = "注入原始消息失败";
        return false;
    }
    return true;
}

bool RemoteSession::release_frame(uint32_t frame_id, uint32_t status,
                                  std::string* error) {
    if (!is_serving_) {
        if (error) *error = "仅 Serve 角色可显式回送 FRAME_RELEASE";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retained_frames_.erase(frame_id);
    }
    KopmsFrameReleasePayload release{};
    release.struct_size = KOPMS_FRAME_RELEASE_PAYLOAD_SIZE;
    release.frame_id = frame_id;
    release.status = status;
    return send_message(KOPMS_MESSAGE_FLAG_MEDIA, KOPMS_MESSAGE_FRAME_RELEASE,
                        KOPMS_MESSAGE_FLAG_MEDIA, next_send_sequence(),
                        codec::encode_frame_release(release), {}, error);
}

void RemoteSession::disconnect() {
    if (lane_ && hello_complete_.load() && !done_.load()) {
        KopmsGoodbyePayload goodbye{};
        goodbye.struct_size = KOPMS_GOODBYE_PAYLOAD_SIZE;
        goodbye.reason = 0;
        send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_GOODBYE,
                     KOPMS_MESSAGE_FLAG_CONTROL, next_send_sequence(),
                     codec::encode_goodbye(goodbye), {}, nullptr);
    }
    if (lane_) {
        lane_->stop();
        lane_.reset();
    }
    FrameReleaseObserver observer;
    std::vector<std::pair<uint32_t, uint32_t>> releases;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observer = frame_release_observer_;
        for (const auto& kv : pending_frames_) {
            releases.emplace_back(kv.first, KOPMS_FRAME_RELEASE_DROPPED);
        }
        pending_frames_.clear();
        control_acks_.clear();
        release_status_.clear();
        retained_frames_.clear();
        pending_control_sequences_.clear();
        control_channel_ = 0;
        media_channel_ = 0;
        hello_complete_.store(false);
        done_.store(true);
    }
    cv_.notify_all();
    if (observer) {
        for (const auto& r : releases) observer(r.first, r.second);
    }
}

void RemoteSession::set_frame_release_observer(FrameReleaseObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_release_observer_ = std::move(observer);
}

void RemoteSession::set_control_ack_observer(ControlAckObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    control_ack_observer_ = std::move(observer);
}

void RemoteSession::set_control_request_handler(ControlRequestHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    control_request_handler_ = std::move(handler);
}

void RemoteSession::set_frame_handler(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_handler_ = std::move(handler);
}

void RemoteSession::set_error_observer(ErrorObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_observer_ = std::move(observer);
}

void RemoteSession::set_reconnect_observer(ReconnectObserver observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_observer_ = std::move(observer);
}

void RemoteSession::on_transport_state(bool connected) {
    if (connected) {
        // 传输恢复（重连成功）：通道已由承载层重开，但 KOPMS 协商必须重来。
        ReconnectObserver observer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 新连接的对端是新会话：上一条连接的序号空间整体作废，
            // 收发序号回到初始值，否则重连后首轮 HELLO/HELLO_ACK 会被
            // “序号未严格递增”误拒（见 handle_message 的序号校验）。
            send_sequence_ = 1;
            last_recv_sequence_ = 0;
            // 仅在“曾完成过协商”时通知应用重新协商；首次连接由应用自行 hello()
            if (!negotiated_once_) return;
            observer = reconnect_observer_;
        }
        if (observer) observer();
        return;
    }
    // 断开：在途帧/控制请求一律判为丢失，协商状态作废
    const bool permanent = !lane_ || lane_->done();
    FrameReleaseObserver release_observer;
    std::vector<std::pair<uint32_t, uint32_t>> releases;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hello_complete_.store(false);
        peer_caps_ = 0;
        negotiated_caps_ = 0;
        negotiated_minor_ = KOPMS_PROTOCOL_MINOR;
        release_observer = frame_release_observer_;
        for (const auto& kv : pending_frames_) {
            releases.emplace_back(kv.first, KOPMS_FRAME_RELEASE_DROPPED);
        }
        pending_frames_.clear();
        control_acks_.clear();
        release_status_.clear();
        retained_frames_.clear();
        pending_control_sequences_.clear();
        if (permanent) {
            control_channel_ = 0;
            media_channel_ = 0;
            done_.store(true);
        }
    }
    cv_.notify_all();
    // 观察者在锁外触发：应用可能据此重新 hello/提交帧（回调里会回调本对象）
    if (release_observer) {
        for (const auto& r : releases) release_observer(r.first, r.second);
    }
}

// ---- lane 路由 ----

void RemoteSession::on_channel_open(uint32_t id, const std::string& kind,
                                    ChannelMode mode) {
    if (mode != ChannelMode::Stream) {
        log("拒绝非 Stream 的 KOPMS lane: " + kind);
        lane_->close_channel(id);
        return;
    }
    const bool is_control = kind == kKopmsControlLane;
    const bool is_media = kind == kKopmsMediaLane;
    if (!is_control && !is_media) {
        log("拒绝未知 KOPMS lane kind: " + kind);
        lane_->close_channel(id);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t& slot = is_control ? control_channel_ : media_channel_;
        if (slot != 0 && slot != id) {
            log("重复的 KOPMS lane，拒绝后开通道");
            lane_->close_channel(id);
            return;
        }
        slot = id;
    }
    if (is_control) {
        lane_->set_data_handler(
            id, [this](uint32_t channel, const std::vector<uint8_t>& data,
                       const std::vector<int>& fds) { on_control_data(channel, data, fds); });
    } else {
        lane_->set_data_handler(
            id, [this](uint32_t channel, const std::vector<uint8_t>& data,
                       const std::vector<int>& fds) { on_media_data(channel, data, fds); });
    }
}

void RemoteSession::on_control_data(uint32_t /*channel*/,
                                    const std::vector<uint8_t>& data,
                                    const std::vector<int>& fds) {
    KopmsMessageHeader header{};
    std::vector<uint8_t> payload;
    if (!split_message(data, fds, &header, &payload)) return;
    handle_message(KOPMS_MESSAGE_FLAG_CONTROL, header, payload, fds);
}

void RemoteSession::on_media_data(uint32_t /*channel*/,
                                  const std::vector<uint8_t>& data,
                                  const std::vector<int>& fds) {
    KopmsMessageHeader header{};
    std::vector<uint8_t> payload;
    if (!split_message(data, fds, &header, &payload)) return;
    handle_message(KOPMS_MESSAGE_FLAG_MEDIA, header, payload, fds);
}

bool RemoteSession::split_message(const std::vector<uint8_t>& data,
                                  const std::vector<int>& fds,
                                  KopmsMessageHeader* header,
                                  std::vector<uint8_t>* payload) {
    if (data.size() < KOPMS_PROTOCOL_HEADER_SIZE) {
        send_error(KOPMS_ERROR_MALFORMED, 0, "消息短于协议头");
        return false;
    }
    std::string decode_err;
    if (!codec::decode_message_header(data.data(), data.size(), header, &decode_err)) {
        send_error(KOPMS_ERROR_MALFORMED, 0, decode_err);
        return false;
    }
    const size_t end = KOPMS_PROTOCOL_HEADER_SIZE + header->payload_size;
    if (end > data.size()) {
        send_error(KOPMS_ERROR_MALFORMED, header->type, "payload 被截断");
        return false;
    }
    if (header->fd_count != fds.size()) {
        send_error(KOPMS_ERROR_MALFORMED, header->type,
                   "fd 数量与协议头不符（期望 " + std::to_string(header->fd_count) +
                       " 收到 " + std::to_string(fds.size()) + "）");
        return false;
    }
    payload->assign(data.begin() + KOPMS_PROTOCOL_HEADER_SIZE, data.begin() + end);
    return true;
}

void RemoteSession::handle_message(uint16_t lane, const KopmsMessageHeader& header,
                                   const std::vector<uint8_t>& payload,
                                   const std::vector<int>& fds) {
    if (header.sequence == 0 || header.sequence <= last_recv_sequence_) {
        send_error(KOPMS_ERROR_SEQUENCE, header.type, "序列号未严格递增");
        return;
    }
    last_recv_sequence_ = header.sequence;

    if (header.type == KOPMS_MESSAGE_HELLO) {
        if (is_serving_) handle_hello(payload);
        else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_HELLO, "Connect 角色不接受 HELLO");
        return;
    }
    if (header.type == KOPMS_MESSAGE_HELLO_ACK) {
        if (!is_serving_) handle_hello_ack(header, payload);
        else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_HELLO_ACK, "Serve 角色不接受 HELLO_ACK");
        return;
    }
    if (!hello_complete_.load()) {
        send_error(KOPMS_ERROR_SEQUENCE, header.type, "HELLO 之前收到业务消息");
        return;
    }

    switch (header.type) {
        case KOPMS_MESSAGE_CONTROL_COMMAND:
            if (is_serving_) handle_control_command(header, payload);
            else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_CONTROL_COMMAND,
                            "Connect 角色不处理控制请求");
            break;
        case KOPMS_MESSAGE_CONTROL_ACK:
            if (!is_serving_) handle_control_ack(header, payload);
            else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_CONTROL_ACK,
                            "Serve 角色不处理 CONTROL_ACK");
            break;
        case KOPMS_MESSAGE_FRAME_SUBMIT:
            if (is_serving_) handle_frame_submit(header, payload, fds);
            else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_FRAME_SUBMIT,
                            "Connect 角色不接收帧");
            break;
        case KOPMS_MESSAGE_FRAME_RELEASE:
            if (!is_serving_) handle_frame_release(payload);
            else send_error(KOPMS_ERROR_UNSUPPORTED, KOPMS_MESSAGE_FRAME_RELEASE,
                            "Serve 角色不接收 FRAME_RELEASE");
            break;
        case KOPMS_MESSAGE_PING:
            send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_PONG,
                         KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                         next_send_sequence(), {}, {}, nullptr);
            break;
        case KOPMS_MESSAGE_PONG:
            break;  // 保活应答：无状态
        case KOPMS_MESSAGE_GOODBYE:
            // Serve 角色被客户端告别：会话结束。Connect 角色被服务端告别
            // 只代表“对端要结束当前这条连接”——随后传输会返回 Closed，
            // 由 on_transport_state 统一决定是重连（auto_reconnect）还是永久
            // 结束（此时 close handler 会调 mark_done）。若在此直接 mark_done，
            // 可重连会话将永远无法恢复。
            if (is_serving_) mark_done();
            break;
        case KOPMS_MESSAGE_ERROR: {
            KopmsErrorCode code = KOPMS_ERROR_MALFORMED;
            uint16_t offending = 0;
            std::string message;
            std::string decode_err;
            if (codec::decode_error(payload, &code, &offending, &message, &decode_err)) {
                set_last_error(message);
                ErrorObserver observer;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    observer = error_observer_;
                }
                if (observer) observer(code, message);
            }
            mark_done();
            break;
        }
        default:
            send_error(KOPMS_ERROR_MALFORMED, header.type, "未知消息类型");
            break;
    }
    (void)lane;
}

void RemoteSession::handle_hello(const std::vector<uint8_t>& payload) {
    KopmsHelloPayload hello{};
    std::string decode_err;
    if (!codec::decode_hello(payload, &hello, &decode_err)) {
        send_error(KOPMS_ERROR_MALFORMED, KOPMS_MESSAGE_HELLO, decode_err);
        return;
    }
    if (hello.protocol_major != KOPMS_PROTOCOL_MAJOR) {
        send_error(KOPMS_ERROR_VERSION, KOPMS_MESSAGE_HELLO, "协议 major 不一致");
        return;
    }
    const uint64_t server_caps = opts_.server_capabilities
                                     ? opts_.server_capabilities
                                     : kDefaultServerCapabilities;
    KopmsHelloAckPayload ack{};
    ack.struct_size = KOPMS_HELLO_ACK_PAYLOAD_SIZE;
    ack.status = 0;
    ack.selected_major = KOPMS_PROTOCOL_MAJOR;
    ack.selected_minor = static_cast<uint16_t>(
        std::min(static_cast<uint32_t>(hello.protocol_minor),
                 static_cast<uint32_t>(KOPMS_PROTOCOL_MINOR)));
    ack.max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    ack.max_fds = KOPMS_PROTOCOL_MAX_FDS;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_caps_ = hello.capabilities;
        negotiated_caps_ = server_caps & hello.capabilities;
        negotiated_minor_ = ack.selected_minor;
        local_caps_ = server_caps;
        ack.capabilities = negotiated_caps_;
        hello_complete_.store(true);
    }
    cv_.notify_all();
    send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_HELLO_ACK,
                 KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                 next_send_sequence(), codec::encode_hello_ack(ack), {}, nullptr);
}

void RemoteSession::handle_hello_ack(const KopmsMessageHeader& header,
                                     const std::vector<uint8_t>& payload) {
    if (header.flags != (KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL)) {
        set_last_error("HELLO_ACK 标志位非法");
        mark_done();
        return;
    }
    KopmsHelloAckPayload ack{};
    std::string decode_err;
    if (!codec::decode_hello_ack(payload, &ack, &decode_err)) {
        set_last_error("非法的 HELLO_ACK: " + decode_err);
        mark_done();
        return;
    }
    if (ack.status != 0 || ack.selected_major != KOPMS_PROTOCOL_MAJOR ||
        ack.selected_minor > KOPMS_PROTOCOL_MINOR ||
        ack.max_payload < KOPMS_HELLO_ACK_PAYLOAD_SIZE ||
        ack.max_payload > KOPMS_PROTOCOL_MAX_PAYLOAD) {
        set_last_error("HELLO_ACK 协商结果不可接受");
        mark_done();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer_caps_ = ack.capabilities;
        negotiated_caps_ = local_caps_ & ack.capabilities;
        negotiated_minor_ = ack.selected_minor;
        max_fds_ = ack.max_fds;
        hello_complete_.store(true);
        negotiated_once_ = true;
    }
    cv_.notify_all();
}

void RemoteSession::handle_control_command(const KopmsMessageHeader& header,
                                           const std::vector<uint8_t>& payload) {
    KopmsControlCommandPayload command{};
    std::vector<uint8_t> data;
    std::string decode_err;
    if (!codec::decode_control_command(payload, &command, &data, &decode_err)) {
        send_error(KOPMS_ERROR_MALFORMED, header.type, decode_err);
        return;
    }
    KopmsControlAckPayload ack{};
    ack.struct_size = KOPMS_CONTROL_ACK_PAYLOAD_SIZE;
    ack.operation = command.operation;
    ack.request_sequence = header.sequence;
    ack.object_id = command.object_id;
    ack.related_id = command.related_id;
    ControlRequestHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = control_request_handler_;
    }
    ack.status = handler ? static_cast<uint32_t>(handler(command, data, &ack))
                         : KOPMS_CONTROL_STATUS_OK;
    send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_CONTROL_ACK,
                 KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                 next_send_sequence(), codec::encode_control_ack(ack), {}, nullptr);
}

void RemoteSession::handle_control_ack(const KopmsMessageHeader& /*header*/,
                                       const std::vector<uint8_t>& payload) {
    KopmsControlAckPayload ack{};
    std::string decode_err;
    if (!codec::decode_control_ack(payload, &ack, &decode_err)) {
        send_error(KOPMS_ERROR_MALFORMED, KOPMS_MESSAGE_CONTROL_ACK, decode_err);
        return;
    }
    ControlAckObserver observer;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto it = pending_control_sequences_.find(ack.request_sequence);
        if (it == pending_control_sequences_.end()) {
            lock.unlock();
            send_error(KOPMS_ERROR_SEQUENCE, KOPMS_MESSAGE_CONTROL_ACK,
                       "收到未知请求的 CONTROL_ACK");
            return;
        }
        pending_control_sequences_.erase(it);
        control_acks_.emplace(ack.request_sequence, ack);
        observer = control_ack_observer_;
        lock.unlock();
    }
    cv_.notify_all();
    if (observer) observer(ack);
}

void RemoteSession::handle_frame_submit(const KopmsMessageHeader& /*header*/,
                                        const std::vector<uint8_t>& payload,
                                        const std::vector<int>& fds) {
    KopmsFrameSubmitPayload frame{};
    std::string decode_err;
    if (!codec::decode_frame_submit(payload, &frame, &decode_err) ||
        !codec::validate_frame_submit_caps(frame, fds.size(), negotiated_caps_, &decode_err)) {
        close_owned_fds(fds);
        send_error(KOPMS_ERROR_FRAME, KOPMS_MESSAGE_FRAME_SUBMIT, decode_err);
        return;
    }
    RemoteFrame received;
    received.payload = frame;
    received.fds = fds;  // fd 所有权随本对象移交 frame handler
    FrameHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = frame_handler_;
    }
    const FrameDisposition disposition =
        handler ? handler(std::move(received)) : FrameDisposition::Release;
    const uint32_t status = release_status_of(disposition);
    if (status == UINT32_MAX) {
        // Retain：handler 已 std::move 取走帧；回送由 release_frame() 承担。
        std::lock_guard<std::mutex> lock(mutex_);
        retained_frames_.emplace(frame.frame_id, frame);
        return;
    }
    release_frame(frame.frame_id, status, nullptr);
}

void RemoteSession::handle_frame_release(const std::vector<uint8_t>& payload) {
    KopmsFrameReleasePayload release{};
    std::string decode_err;
    if (!codec::decode_frame_release(payload, &release, &decode_err)) {
        send_error(KOPMS_ERROR_MALFORMED, KOPMS_MESSAGE_FRAME_RELEASE, decode_err);
        return;
    }
    FrameReleaseObserver observer;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto it = pending_frames_.find(release.frame_id);
        if (it != pending_frames_.end()) pending_frames_.erase(it);
        release_status_.emplace(release.frame_id, release.status);
        observer = frame_release_observer_;
        lock.unlock();
    }
    cv_.notify_all();
    if (observer) observer(release.frame_id, release.status);
}

bool RemoteSession::send_message(uint16_t lane, uint16_t type, uint16_t flags,
                                 uint64_t sequence,
                                 const std::vector<uint8_t>& payload,
                                 const std::vector<int>& fds,
                                 std::string* error) {
    if (!lane_) {
        if (error) *error = "会话不存在";
        return false;
    }
    const uint32_t channel =
        (lane == KOPMS_MESSAGE_FLAG_MEDIA) ? media_channel_ : control_channel_;
    if (channel == 0) {
        if (error) *error = "对应 lane 尚未建立";
        return false;
    }
    KopmsMessageHeader header{};
    header.magic = KOPMS_PROTOCOL_MAGIC;
    header.major = KOPMS_PROTOCOL_MAJOR;
    header.minor = KOPMS_PROTOCOL_MINOR;
    header.type = type;
    header.flags = flags;
    header.sequence = sequence;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.fd_count = static_cast<uint32_t>(fds.size());
    codec::WireHeader wire{};
    codec::encode_message_header(header, &wire);
    std::vector<uint8_t> bytes(wire.begin(), wire.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const SendStatus st = lane_->send(channel, bytes.data(), bytes.size(),
                                         fds.empty() ? nullptr : &fds,
                                         opts_.send_timeout_ms);
    if (st != SendStatus::Ok) {
        if (error)
            *error = std::string("隧道发送失败: ") +
                     (st == SendStatus::Timeout ? "超时（credit/背压）" : "通道关闭或错误");
        return false;
    }
    return true;
}

bool RemoteSession::send_error(KopmsErrorCode code, uint16_t offending_type,
                               const std::string& message) {
    set_last_error(message);
    return send_message(KOPMS_MESSAGE_FLAG_CONTROL, KOPMS_MESSAGE_ERROR,
                        KOPMS_MESSAGE_FLAG_REPLY | KOPMS_MESSAGE_FLAG_CONTROL,
                        next_send_sequence(),
                        codec::encode_error(code, offending_type, message), {}, nullptr);
}

void RemoteSession::set_last_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = message;
}

uint64_t RemoteSession::next_send_sequence() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t current = send_sequence_++;
    if (send_sequence_ == 0) send_sequence_ = 1;
    return current == 0 ? next_send_sequence() : current;
}

void RemoteSession::mark_done() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_.store(true);
    }
    cv_.notify_all();
}

void RemoteSession::release_pending_frames(uint32_t /*status*/) {
    // 保留给未来需要在停机时批量通知的场景；当前 disconnect() 内联处理。
}

void RemoteSession::log(const std::string& message) const {
    if (logger_) {
        logger_(message);
        return;
    }
    KOP_LOG_INFO(kTag, "%s", message.c_str());
}

}  // namespace kopnet
