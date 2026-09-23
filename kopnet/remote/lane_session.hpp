// RemoteSession 的通道承载抽象：把“裸隧道会话”与“可重连会话”统一成同一组
// 操作，RemoteSession 的业务逻辑（KOPMS 线协议、帧状态机）因此不感知底层
// 是否具备断线重连能力。
//
//   - PlainLaneSession   包裹一条 TunnelSession（connect 侧单次拨号 / serve
//                        侧接受的会话）。通道号是隧道上的线上号。
//   - ResilientLaneSession  委托给 ResilientSession。通道号是逻辑号，跨重连
//                        保持不变；数据处理器随重连自动重装。
//
// connected()/done() 的语义对两种实现一致：
//   - connected()  传输当前是否连通（重连会话在断开期间为 false）
//   - done()       是否已彻底终止（不会再有重连）
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kopnet/transport.hpp"
#include "kopnet/tunnel.hpp"

namespace kopnet {

class ResilientSession;

class LaneSession {
public:
    using DataHandler = ::kopnet::DataHandler;
    using ChannelHandler = ::kopnet::ChannelHandler;
    using CloseHandler = ::kopnet::CloseHandler;
    using Logger = ::kopnet::TunnelLogger;

    virtual ~LaneSession() = default;

    // 返回通道号（>0）；0 = 失败（error 已填）。handler 可为空。
    virtual uint32_t open_channel(const std::string& kind, ChannelMode mode,
                                  int timeout_ms, DataHandler handler,
                                  std::string* error) = 0;
    virtual void set_channel_handler(ChannelHandler handler) = 0;
    virtual void set_data_handler(uint32_t channel, DataHandler handler) = 0;
    virtual void close_channel(uint32_t channel) = 0;
    virtual void set_close_handler(CloseHandler handler) = 0;
    virtual void set_logger(Logger logger) = 0;
    // serve 角色：注册完全部回调后启动会话（resilient 实现恒已在 start() 时启动）
    virtual bool start(std::string* error) = 0;
    virtual SendStatus send(uint32_t channel, const uint8_t* data, size_t len,
                            const std::vector<int>* fds, int timeout_ms) = 0;
    // 阻塞等待首个连接建立（plain 实现恒为已连接时立即返回 true）
    virtual bool wait_connected(int timeout_ms) = 0;
    virtual bool connected() const = 0;
    virtual bool done() const = 0;
    virtual bool supports_fds() const = 0;
    virtual void stop() = 0;
};

// 包裹一条已建立的裸隧道会话（connect 侧由 tunnel_dial 建立 / serve 侧接受）。
class PlainLaneSession final : public LaneSession {
public:
    explicit PlainLaneSession(std::unique_ptr<TunnelSession> session)
        : session_(std::move(session)) {}
    ~PlainLaneSession() final { stop(); }

    uint32_t open_channel(const std::string& kind, ChannelMode mode, int timeout_ms,
                          DataHandler handler, std::string* error) final {
        if (handler) return session_->open_channel(kind, mode, timeout_ms, handler, error);
        return session_->open_channel(kind, mode, timeout_ms, error);
    }
    void set_channel_handler(ChannelHandler handler) final {
        session_->set_channel_handler(std::move(handler));
    }
    void set_data_handler(uint32_t channel, DataHandler handler) final {
        session_->set_data_handler(channel, std::move(handler));
    }
    void close_channel(uint32_t channel) final { session_->close_channel(channel); }
    void set_close_handler(CloseHandler handler) final {
        session_->set_close_handler(std::move(handler));
    }
    void set_logger(Logger logger) final { session_->set_logger(std::move(logger)); }
    SendStatus send(uint32_t channel, const uint8_t* data, size_t len,
                    const std::vector<int>* fds, int timeout_ms) final {
        return session_->send(channel, data, len, fds, timeout_ms);
    }
    bool wait_connected(int /*timeout_ms*/) final { return session_->alive(); }
    bool start(std::string* error) final { return session_->start(error); }
    bool connected() const final { return session_->alive(); }
    bool done() const final { return !session_->alive(); }
    bool supports_fds() const final { return session_->transport()->supports_fds(); }
    void stop() final { session_->stop(); }

private:
    std::unique_ptr<TunnelSession> session_;
};

// 委托给 ResilientSession：通道号为逻辑号，断线自动重连/故障转移。
class ResilientLaneSession final : public LaneSession {
public:
    explicit ResilientLaneSession(std::unique_ptr<ResilientSession> session)
        : session_(std::move(session)) {}
    ~ResilientLaneSession() final { stop(); }

    uint32_t open_channel(const std::string& kind, ChannelMode mode, int timeout_ms,
                          DataHandler handler, std::string* error) final;
    void set_channel_handler(ChannelHandler handler) final;
    void set_data_handler(uint32_t channel, DataHandler handler) final;
    void close_channel(uint32_t channel) final;
    void set_close_handler(CloseHandler handler) final;
    void set_logger(Logger logger) final;
    SendStatus send(uint32_t channel, const uint8_t* data, size_t len,
                    const std::vector<int>* fds, int timeout_ms) final;
    bool wait_connected(int timeout_ms) final;
    bool start(std::string* /*error*/) final { return true; }
    bool connected() const final;
    bool done() const final;
    bool supports_fds() const final;
    void stop() final;

private:
    std::unique_ptr<ResilientSession> session_;
};

}  // namespace kopnet
