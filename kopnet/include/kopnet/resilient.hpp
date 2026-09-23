// KOPNET 会话韧性层：断线自动重连 + Endpoint 列表故障转移。
//
// TunnelSession 绑死在单条 Transport 上：传输一断（WiFi 抖动、SSH 被杀、
// 中继重启）会话即死，应用只能整体重建。ResilientSession 在其之上加一个
// 连接线程：断开后按指数退避重拨 Endpoint 列表（按优先级故障转移），握手
// 成功后把已注册的通道全部重开。应用只面对**逻辑通道号**——跨重连保持不变：
//   - open_channel()   注册通道；已连接时立即打开，未连接时延迟到连接建立
//   - send()           断开期间立即返回 Closed（不阻塞、不丢线程）
//   - recv()           断开期间停在条件变量上等重连，恢复后从新通道继续收
//   - set_state_handler()  断开/恢复通知，应用可据此刷新状态（如 KOPMS
//                       把在途帧标记为 DROPPED）
//   - set_channel_handler() 首开与每次重连后重开都回调
//
// 重连对服务端透明：每次重连就是一次新会话，服务端代码不用改。
// 语义代价：断开瞬间在途的消息会丢失（与 TCP 断开的语义一致），重连后
// 从新通道继续；上层需要端到端语义（如 KOPMS 的帧 id）自行重放。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kopnet/tunnel.hpp"

namespace kopnet {

class ResilientSession {
public:
    struct Options {
        // 按优先级排序的故障转移列表：前面的拨不通才试后面的（每次重连
        // 都从列表头开始，保证主路径恢复后自动切回）
        std::vector<std::string> endpoints;
        uint32_t base_delay_ms = 500;   // 首次重连延迟（后续指数退避）
        uint32_t max_delay_ms = 5000;   // 退避上限
        int connect_timeout_ms = 5000;  // 单次拨号的 HELLO 握手超时
        uint32_t max_attempts = 0;      // 0 = 无限重连；否则耗尽后停止
        TunnelSession::Options tunnel_opts;
    };

    using ChannelHandler =
        std::function<void(uint32_t logical, const std::string& kind, ChannelMode mode)>;
    using StateHandler = std::function<void(bool connected)>;
    using DataHandler = ::kopnet::DataHandler;
    using CloseHandler = ::kopnet::CloseHandler;
    using TunnelLogger = ::kopnet::TunnelLogger;

    ResilientSession();
    explicit ResilientSession(const Options& opts);
    ~ResilientSession();

    // 必须在 start() 之前调用
    void set_options(Options opts);

    ResilientSession(const ResilientSession&) = delete;
    ResilientSession& operator=(const ResilientSession&) = delete;

    // 启动连接线程（首次连接异步进行；用 wait_connected 等首连）。
    // endpoints 为空时返回错误。
    bool start(std::string* error);
    // 停止连接线程并回收资源（幂等；可在析构外的任何线程调用）
    void stop();

    // 注册一个逻辑通道，返回逻辑号（>0，跨重连不变）。已连接时立即在对端
    // 打开；未连接时由连接线程在建立后打开。打开失败不返回 0——逻辑号总是
    // 分配，通道在下次连接时重试（失败信息经 error 带回）。
    uint32_t open_channel(const std::string& kind, ChannelMode mode, std::string* error);
    // 同上，但 data handler 随通道注册一并装好（推荐 push 模型使用：
    // handler 存在逻辑通道上，每次重连随通道重开自动重装，无投递窗口）。
    uint32_t open_channel(const std::string& kind, ChannelMode mode, int timeout_ms,
                          DataHandler handler, std::string* error);
    // 给已注册的逻辑通道更换数据处理器（重连后仍生效）。
    void set_data_handler(uint32_t logical, DataHandler handler);
    // 关闭逻辑通道：标记后不再随重连重开，当前连接上的通道立即关闭
    void close_channel(uint32_t logical);
    void set_channel_handler(ChannelHandler h);
    void set_state_handler(StateHandler h);
    // 会话永久终止（重连耗尽或 stop）时回调一次；handler 内不得调 stop()
    void set_close_handler(CloseHandler h);
    // 内部隧道会话的日志路由（默认走 KOP_LOG）
    void set_logger(TunnelLogger h);

    SendStatus send(uint32_t logical, const uint8_t* data, size_t len,
                    const std::vector<int>* fds, int timeout_ms);
    // timeout_ms>0：断开期间阻塞等待重连（总等待不超过 timeout_ms）。
    RecvStatus recv(uint32_t logical, std::vector<uint8_t>* data,
                    std::vector<int>* fds, uint32_t timeout_ms);

    bool connected() const;
    // 重连线程是否已退出（stop() 或重连次数耗尽）。
    // 对节点层即“会话已死，不再尝试”的信号
    bool reconnect_done() const;
    // 当前会话的传输是否支持 fd 透传（断开时返回 false）
    bool supports_fds() const;
    // 等待首个连接建立（已连接时立即返回 true；超时返回 false）
    bool wait_connected(int timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kopnet
