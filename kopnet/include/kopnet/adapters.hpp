// KOPNET 协议层：适配器注册表 + 便捷入口。
//
// 每个 scheme 注册一个 ProtocolAdapter，负责“把该协议的一条连接变成
// 一个 Transport”。注册表是全局单例，内置 tcp/udp/unix/ssh/relay/rtp；
// rtc/rdp 留出注册点，由后续里程碑实现（先返回“未实现”错误，
// 保证 URI 层可以先跑通）。
//
// tunnel_dial / TunnelServer 是应用层最常用的两个入口：吃 URI，吐
// 已协商好的 TunnelSession。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kopnet/endpoint.hpp"
#include "kopnet/transport.hpp"
#include "kopnet/transport_listener.hpp"
#include "kopnet/tunnel.hpp"

namespace kopnet {

struct ProtocolAdapter {
    std::string scheme;
    // Connect 角色：建立连接并返回 Transport
    std::function<std::unique_ptr<Transport>(const Endpoint& ep, std::string* error)> dial;
    // Serve 角色（可为空，表示该 scheme 不支持监听）
    std::function<std::unique_ptr<TransportListener>(const Endpoint& ep,
                                                     std::string* error)> serve;
};

class AdapterRegistry {
public:
    static AdapterRegistry& instance();

    // 注册/覆盖某 scheme 的 adapter。返回是否成功。
    bool register_adapter(ProtocolAdapter adapter);

    // 解析 URI 并按 scheme 建立连接。
    bool dial(const std::string& uri, std::unique_ptr<Transport>* out,
              std::string* error) const;
    // 解析 URI 并创建监听器。
    bool serve(const std::string& uri, std::unique_ptr<TransportListener>* out,
               std::string* error) const;

    bool has_scheme(const std::string& scheme) const;
    std::vector<std::string> schemes() const;

private:
    AdapterRegistry();
    void register_builtin();

    std::vector<ProtocolAdapter> adapters_;
};

// 便捷：URI → 已完成 HELLO 协商的隧道会话（Connect 角色）。
std::unique_ptr<TunnelSession> tunnel_dial(const std::string& uri,
                                           TunnelSession::Options opts, std::string* error);

// 便捷：在 URI 上 Serve，每条接入连接经 HELLO 后回调（Serve 角色）。
// 回调在 run() 线程中执行；session 的生命周期由回调持有者决定。
class TunnelServer {
public:
    using Handler = std::function<void(std::unique_ptr<TunnelSession> session)>;

    bool listen(const std::string& uri, std::string* error);
    // 阻塞接受连接；stop() 可从其它线程唤醒退出。
    void run(Handler handler);
    void stop();
    ~TunnelServer();

private:
    std::unique_ptr<TransportListener> listener_;
    std::atomic<bool> stop_{false};
};

}  // namespace kopnet
