// KOPNET 远程会话：把 KOPMS-S/KOPMS-C 的线协议搬到 KOPNET 隧道通道上。
//
// BUS2LAYER 的线语义完全不变（32B 小端头 + payload + 按 fd_index 引用的
// SCM_RIGHTS），承载从本地 unix seqpacket 换成一对隧道通道：
//
//   kopms.control（Stream） HELLO / HELLO_ACK / ERROR / PING / PONG /
//                           GOODBYE / CONTROL_COMMAND / CONTROL_ACK
//   kopms.media   （Stream） FRAME_SUBMIT（可带 fd）/ FRAME_RELEASE
//
// 一条 KOPMS 消息恒为一次隧道发送，故 fd 与消息的归属在隧道的流式分帧里
// 依然精确（fd 只随某一次 recvmsg 到达，且只有整条消息才构成一次发送）。
//
// 应用层因此拿到与 kopms::KopmsClient 一致的提交/控制接口，但对端可以在
// ssh://、tcp://、relay:// 的另一头；socket、序列化与加密细节被隧道与
// 适配器层完全吸收（SSH 由远端 kopnet-relay --stdio 伙伴承担）。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "frame_bridge.h"
#include "kopms_protocol.h"

#include "kopnet/tunnel.hpp"

namespace kopnet {

class LaneSession;  // 实现见 remote/lane_session.hpp

// 通道 kind：两条 lane 的划分与协议头的 CONTROL/MEDIA 标志一致。
inline constexpr const char* kKopmsControlLane = "kopms.control";
inline constexpr const char* kKopmsMediaLane = "kopms.media";

// serve 侧收到的帧：payload.planes[i].fd_index / acquire_fence.fd_index
// 直接索引 fds（SCM_RIGHTS 随同一条隧道帧到达）。析构关闭全部 fd——
// 接收侧拥有对端移交的 fd 引用副本。
struct RemoteFrame {
    KopmsFrameSubmitPayload payload{};
    std::vector<int> fds;

    RemoteFrame() = default;
    ~RemoteFrame();
    RemoteFrame(const RemoteFrame&) = delete;
    RemoteFrame& operator=(const RemoteFrame&) = delete;
    RemoteFrame(RemoteFrame&& other) noexcept;
    RemoteFrame& operator=(RemoteFrame&& other) noexcept;

    void close_fds();
};

// serve 侧对 FRAME_SUBMIT 的处置，决定回送的 FRAME_RELEASE 状态。
enum class FrameDisposition : uint32_t {
    Release = 0,         // 已处理完毕 → FRAME_RELEASE_OK
    Retain = 1,          // 暂不回送：handler 必须 std::move 取走帧（含 fd 所有权），
                         // 稍后由应用调 release_frame() 回送
    ReleaseDropped = 2,  // 回压拒收 → FRAME_RELEASE_DROPPED
    ReleaseRejected = 3, // 契约失败 → FRAME_RELEASE_REJECTED
};

class RemoteSession {
public:
    struct Options {
        int hello_timeout_ms = 3000;  // KOPMS HELLO / HELLO_ACK 等待上限
        int send_timeout_ms = 2000;   // 通道发送（credit 排队）上限
        // serve 侧声明的本地能力（HELLO_ACK 回送）；0 表示支持全部已定义能力。
        uint64_t server_capabilities = 0;
        // ---- Connect 角色：会话韧性（默认关闭，保持旧行为）----
        // 开启后传输断开时自动重连并按 fallback_endpoints 故障转移，
        // 逻辑通道（kopms.control / kopms.media）跨重连保持。
        bool auto_reconnect = false;
        std::vector<std::string> fallback_endpoints;  // 备用端点，按优先级
        uint32_t reconnect_base_delay_ms = 500;       // 首次重连延迟
        uint32_t reconnect_max_delay_ms = 5000;       // 退避上限
    };

    using FrameReleaseObserver =
        std::function<void(uint32_t frame_id, uint32_t status)>;
    using ControlAckObserver =
        std::function<void(const KopmsControlAckPayload& ack)>;
    // serve 侧：收到 CONTROL_COMMAND 时回填 ack 的 generation / object_id 等
    // 业务字段并返回状态；返回非 OK 仍会回送 CONTROL_ACK（携带该状态）。
    using ControlRequestHandler =
        std::function<KopmsControlStatus(const KopmsControlCommandPayload& command,
                                         const std::vector<uint8_t>& data,
                                         KopmsControlAckPayload* ack)>;
    using FrameHandler = std::function<FrameDisposition(RemoteFrame frame)>;
    using ErrorObserver =
        std::function<void(KopmsErrorCode code, const std::string& message)>;
    // 传输恢复（且此前已完成过 KOPMS 协商）时回调：应用必须重新 hello() 并
    // 重建对端状态（窗口树/焦点等）。首次连接不触发——应用在 open() 后自行 hello()。
    using ReconnectObserver = std::function<void()>;

    RemoteSession();
    ~RemoteSession();

    RemoteSession(const RemoteSession&) = delete;
    RemoteSession& operator=(const RemoteSession&) = delete;

    // ---------- Connect 角色 ----------
    // 拨号端点、开两条 lane 并完成隧道 HELLO。返回后应用再调 hello() 协商
    // KOPMS 能力（与 kopms::KopmsClient 的 connect/hello 两步一致）。
    bool open(const std::string& uri, const Options& opts, std::string* error);
    // 发送 HELLO 并等待 HELLO_ACK；capabilities 是本端想声明的能力位。
    bool hello(uint64_t capabilities, std::string* error);
    // 提交一帧 DMA-BUF/外部内存帧。成功后描述符由本会话保留，直到收到
    // FRAME_RELEASE（回调 FrameReleaseObserver，应用此时才能释放帧）。
    // 失败时不保留，调用方自行处理。
    bool submit(KopmsFrameDescriptor* frame, uint32_t* frame_id, std::string* error);
    // 发送一条控制请求；应答经 ControlAckObserver 或 wait_for_control_ack 取得。
    bool send_control(const KopmsControlCommandPayload& command,
                      const std::vector<uint8_t>& data, uint64_t* request_sequence,
                      std::string* error);
    bool wait_for_control_ack(uint64_t request_sequence, int timeout_ms,
                              KopmsControlAckPayload* ack, std::string* error);
    bool wait_for_release(uint32_t frame_id, int timeout_ms, std::string* error);
    // 保活探测（对端回 PONG）。
    bool ping(std::string* error);
    // 诊断/一致性测试：绕过状态机，把一条**已编码的完整 KOPMS 消息**
    // （32B 头 + payload）直接发到 control lane。生产代码不应使用。
    bool send_raw_control(const std::vector<uint8_t>& message, std::string* error);

    // ---------- Serve 角色 ----------
    // 接管一条**尚未 start** 的隧道会话：先注册通道/数据回调再 start，
    // 保证对端开通道与首帧数据到达时回调已就位（通道回调在 OPEN_ACK 与
    // 额度授予之前触发，见 TunnelSession::set_channel_handler）。
    // 非阻塞；会话由本对象持有，wait_done() 等待会话结束。
    bool serve(std::unique_ptr<TunnelSession> session, const Options& opts,
               std::string* error);
    // 等待会话结束（GOODBYE、传输断开或 disconnect）。timeout_ms<=0 仅检查。
    bool wait_done(int timeout_ms);
    // 对之前以 Retain 取走的帧显式回送 FRAME_RELEASE（fd 由应用关闭）。
    bool release_frame(uint32_t frame_id, uint32_t status, std::string* error);

    // 可能时发送 GOODBYE，停止隧道并释放全部在飞帧。
    void disconnect();

    bool connected() const { return hello_complete_.load(); }
    bool serving() const { return is_serving_; }
    uint64_t local_capabilities() const { return local_caps_; }
    uint64_t peer_capabilities() const { return peer_caps_; }
    uint64_t negotiated_capabilities() const { return negotiated_caps_; }
    uint16_t negotiated_minor() const { return negotiated_minor_; }

    void set_frame_release_observer(FrameReleaseObserver observer);
    void set_control_ack_observer(ControlAckObserver observer);
    void set_control_request_handler(ControlRequestHandler handler);
    void set_frame_handler(FrameHandler handler);
    void set_error_observer(ErrorObserver observer);
    void set_reconnect_observer(ReconnectObserver observer);

private:
    // lane 路由
    void on_channel_open(uint32_t id, const std::string& kind, ChannelMode mode);
    void on_control_data(uint32_t id, const std::vector<uint8_t>& data,
                         const std::vector<int>& fds);
    void on_media_data(uint32_t id, const std::vector<uint8_t>& data,
                       const std::vector<int>& fds);
    // 把隧道 payload 切成协议头 + KOPMS payload，并校验 fd 计数。
    bool split_message(const std::vector<uint8_t>& data, const std::vector<int>& fds,
                       KopmsMessageHeader* header, std::vector<uint8_t>* payload);
    void handle_message(uint16_t lane, const KopmsMessageHeader& header,
                        const std::vector<uint8_t>& payload,
                        const std::vector<int>& fds);
    void handle_hello(const std::vector<uint8_t>& payload);
    void handle_hello_ack(const KopmsMessageHeader& header,
                          const std::vector<uint8_t>& payload);
    void handle_control_command(const KopmsMessageHeader& header,
                                const std::vector<uint8_t>& payload);
    void handle_control_ack(const KopmsMessageHeader& header,
                            const std::vector<uint8_t>& payload);
    void handle_frame_submit(const KopmsMessageHeader& header,
                             const std::vector<uint8_t>& payload,
                             const std::vector<int>& fds);
    void handle_frame_release(const std::vector<uint8_t>& payload);

    bool send_message(uint16_t lane, uint16_t type, uint16_t flags,
                      uint64_t sequence, const std::vector<uint8_t>& payload,
                      const std::vector<int>& fds, std::string* error);
    bool send_error(KopmsErrorCode code, uint16_t offending_type,
                    const std::string& message);
    void set_last_error(const std::string& message);
    uint64_t next_send_sequence();
    void mark_done();
    // 承载层连通状态变化（仅 auto_reconnect 的 Connect 角色）：断开时把在途
    // 帧统一标记为 DROPPED 并重置协商状态；恢复时通知应用重新协商
    void on_transport_state(bool connected);
    void release_pending_frames(uint32_t status);  // 调用方持 mutex_
    void log(const std::string& message) const;

    std::unique_ptr<LaneSession> lane_;
    Options opts_;
    bool is_serving_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint32_t control_channel_ = 0;
    uint32_t media_channel_ = 0;
    std::atomic<bool> hello_complete_{false};
    std::atomic<bool> done_{false};
    uint16_t negotiated_minor_ = KOPMS_PROTOCOL_MINOR;
    uint64_t local_caps_ = 0;
    uint64_t peer_caps_ = 0;
    uint64_t negotiated_caps_ = 0;
    uint64_t send_sequence_ = 1;
    uint64_t last_recv_sequence_ = 0;
    uint32_t next_frame_id_ = 1;
    // connect 侧保留的提交帧（应用拥有，FRAME_RELEASE 后归还）。
    std::unordered_map<uint32_t, KopmsFrameDescriptor*> pending_frames_;
    std::unordered_map<uint64_t, KopmsControlAckPayload> control_acks_;
    std::unordered_map<uint32_t, uint32_t> release_status_;
    // serve 侧以 Retain 交给应用、尚未回送 release 的帧。
    std::unordered_map<uint32_t, KopmsFrameSubmitPayload> retained_frames_;
    std::string last_error_;
    uint32_t max_fds_ = KOPMS_PROTOCOL_MAX_FDS;
    std::unordered_set<uint64_t> pending_control_sequences_;  // 已发未答的控制请求
    FrameReleaseObserver frame_release_observer_;
    ControlAckObserver control_ack_observer_;
    ControlRequestHandler control_request_handler_;
    FrameHandler frame_handler_;
    ErrorObserver error_observer_;
    ReconnectObserver reconnect_observer_;
    // 是否曾完成过 KOPMS 协商：用于区分“首次连接”与“断线后恢复”
    bool negotiated_once_ = false;
    TunnelLogger logger_;
};

}  // namespace kopnet
