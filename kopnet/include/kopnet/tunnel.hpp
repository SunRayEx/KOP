// KOPNET 透明网络架构：隧道（Tunnel）——逻辑通道的多路复用层。
//
// TunnelSession 骑在一条 Transport 上，把它切成若干带编号的逻辑通道。
// 上层（KOPAW 节点 / KOPMS 远程会话 / relay）只面对 Channel 语义：
//   - open_channel(kind, mode)  打开/接受通道
//   - send(id, data[, fds])     发送（credit 回压，bounded 超时）
//   - recv(id) 或 set_data_handler(id, fn)  接收
// 通道的可靠性模式与传输语义在打开时校验：Stream 通道只能落在可靠传输上。
//
// 回压链（与 KOPAW/KOPMS 一致）：应用 send 阻塞于通道发送队列（有界）
//   → 隧道 credit 耗尽暂停写 → 对端收队列满暂停授予 credit
//   → 对端应用 recv 恢复后补发 credit → 本端继续写。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kopnet/transport.hpp"
namespace kopnet {

// 通道可靠性模式：Stream 要求可靠有序传输；Datagram 允许乱序/丢包。
// 选择 Stream 却落在 datagram 语义的传输上时，适配器层会拒绝打开。
#define KOPNET_TUNNEL_DEFAULT_CREDIT 16
#define KOPNET_TUNNEL_MAX_CHANNELS 256
#define KOPNET_TUNNEL_MAX_FRAME (64 * 1024)
#define KOPNET_TUNNEL_MAX_PAYLOAD (KOPNET_TUNNEL_MAX_FRAME - 20)

enum class ChannelMode : uint8_t {
    Datagram = 0,  // 无序、允许丢包（实时媒体/输入事件）
    Stream = 1,    // 有序可靠（控制流/文件传输）
};

enum class SendStatus {
    Ok,
    Closed,
    Timeout,
    Error,
};

enum class RecvStatus {
    Ok,
    Closed,
    Timeout,
    Error,
};

// 数据到达回调（工作线程调用；返回前隧道不再读该通道，天然回压）。
using DataHandler =
    std::function<void(uint32_t channel_id, const std::vector<uint8_t>& data,
                       const std::vector<int>& fds)>;
// 对端打开通道回调（工作线程）。回调在 OPEN_ACK 与额度授予之前触发，
// 应用可在此安装该通道的 data handler，保证首帧数据到达前回调就位。
using ChannelHandler =
    std::function<void(uint32_t id, const std::string& kind, ChannelMode mode)>;
// 会话结束回调（工作线程，在 stop()/join 完成之前触发一次）：传输断开、
// HELLO 超时或停机。handler 内不得调用本会话的 stop()（会自 join 死锁）。
using CloseHandler = std::function<void()>;
using TunnelLogger = std::function<void(const std::string& message)>;

class TunnelSession {
public:
    struct Options {
        uint32_t recv_credit = KOPNET_TUNNEL_DEFAULT_CREDIT;  // 每通道收侧额度/队列深度
        uint32_t max_channels = KOPNET_TUNNEL_MAX_CHANNELS;
        uint32_t max_frame = KOPNET_TUNNEL_MAX_FRAME;
        int handshake_ms = 3000;  // HELLO 协商超时
    };

    // transport 为已建立的传输；is_dialer=true 时本端通道 id 取奇数、对端偶数。
    static std::unique_ptr<TunnelSession> create(std::unique_ptr<Transport> transport,
                                                 bool is_dialer, Options opts,
                                                 std::string* error);
    ~TunnelSession();

    TunnelSession(const TunnelSession&) = delete;
    TunnelSession& operator=(const TunnelSession&) = delete;

    // 启动工作线程并开始 HELLO 协商（立即返回；握手在 open_channel / recv
    // 时按需等待，避免两端必须同时阻塞在 start 的引导问题）。
    bool start(std::string* error);
    // 等待 HELLO 协商完成；timeout_ms<=0 时仅检查当前状态。
    bool wait_hello(int timeout_ms, std::string* error);
    // 通知停止并 join 工作线程（幂等）。
    void stop();
    bool alive() const;

    void set_channel_handler(ChannelHandler h);
    // 设置后该通道数据经回调投递（工作线程）；未设置时用 recv() 拉取。
    void set_data_handler(uint32_t id, DataHandler h);
    void set_close_handler(CloseHandler h);
    void set_logger(TunnelLogger h);

    // 打开通道（阻塞到 OPEN_ACK）。返回通道 id（>0），失败返回 0。
    uint32_t open_channel(const std::string& kind, ChannelMode mode, int timeout_ms,
                          std::string* error);
    // 同上，但 data handler 在 OPEN 发出之前就装到通道上，避免对端首批数据
    // 抢在回调注册之前进入 recv_queue（回调流与队列流乱序）。
    uint32_t open_channel(const std::string& kind, ChannelMode mode, int timeout_ms,
                          DataHandler handler, std::string* error);
    void close_channel(uint32_t id);

    // 发送一帧。fds 仅在支持 fd 透传的传输上有效（见 Transport::supports_fds）。
    SendStatus send(uint32_t id, const uint8_t* data, size_t len,
                    const std::vector<int>* fds, int timeout_ms);
    RecvStatus recv(uint32_t id, std::vector<uint8_t>* data, std::vector<int>* fds,
                    int timeout_ms);

    // 统计与诊断
    uint64_t frames_sent() const;
    uint64_t frames_received() const;
    size_t channel_count() const;
    Transport* transport() const;

private:
    TunnelSession();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kopnet
