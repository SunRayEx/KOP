// KOPNET 应用 SDK：把隧道/协议适配器/韧性层的装配细节封装成一个 facade。
//
// 应用只面对“逻辑通道（按 kind 命名）+ 回调”，不接触 socket、协议或
// 序列化细节——无论对端在本机、SSH 对端还是 RTP 会话：
//
//   // 拨号端
//   kop::sdk::NetTunnelOptions opts;
//   opts.uri = "tcp://127.0.0.1:19700";
//   opts.on_data = [](const std::string& kind, const uint8_t* d, size_t n) { … };
//   auto link = kop::sdk::NetTunnel::open(opts, &err);
//   link->wait_connected(3000);
//   link->open_channel("media", /*ordered=*/false, &err);
//   link->send("media", buf, len, &err);
//
//   // 服务端（同一接口，opts.serve = true）
//   opts.serve = true;
//
// 契约：
//   - 回调（on_data/on_state/on_channel）在内部工作线程调用；返回前隧道
//     不再投递该通道数据（天然回压）。
//   - 本 facade 只搬字节；需要 DMA-BUF 等 fd 透传请用 net 节点
//     （net_source/net_sink，走 SCM_RIGHTS）。
//   - kind 名在本端唯一；服务端按对端打开的通道自动建立同名映射。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kopnet/adapters.hpp"
#include "kopnet/resilient.hpp"

namespace kop {
namespace sdk {

struct NetTunnelOptions {
    std::string uri;  // tcp:// udp:// unix:// ssh:// rtp:// relay:// …

    // true = 在 uri 上监听（Serve 角色）；false = 拨号（Connect 角色）。
    bool serve = false;

    // 数据到达（工作线程；同一通道按到达顺序投递）。
    std::function<void(const std::string& kind, const uint8_t* data, size_t len)> on_data;
    // 连接状态变化：true=已连通，false=断开。
    // 拨号端开启自动重连时，每次断开/恢复都会回调；服务端在会话接入/离开时回调。
    std::function<void(bool connected)> on_state;
    // 对端打开通道时回调（工作线程，仅通知，facade 一律接受）。
    std::function<void(const std::string& kind)> on_channel;
    // 诊断日志（不发则走 KOP_LOG）。
    std::function<void(const std::string&)> on_log;

    // 断线自动重连 + 故障转移（仅拨号端；服务端始终接受新连接）。
    // 关闭后连接一旦断开会话即终止，需重新 open()。
    bool auto_reconnect = true;
    uint32_t reconnect_base_ms = 500;   // 首次重连延迟（后续指数退避）
    uint32_t reconnect_max_ms = 5000;   // 退避上限
    int handshake_ms = 5000;            // HELLO 握手超时
};

struct NetTunnelStats {
    uint64_t frames_sent = 0;
    uint64_t frames_received = 0;
    size_t channels = 0;
    bool connected = false;
};

class NetTunnel {
public:
    ~NetTunnel();
    NetTunnel(const NetTunnel&) = delete;
    NetTunnel& operator=(const NetTunnel&) = delete;

    // 启动连接线程（拨号端）或监听线程（服务端）；连接在后台建立，
    // 用 wait_connected() 等首连。失败返回 nullptr 并填 error。
    static std::unique_ptr<NetTunnel> open(const NetTunnelOptions& options,
                                          std::string* error);

    // 注册并打开一个逻辑通道：kind 名跨重连保持；ordered=true 要求
    // 可靠有序传输（落在 datagram 传输上会被拒绝）。返回通道号（>0），
    // 失败返回 0 并填 error。
    // 服务端角色不支持主动开通道（只能接受对端打开的），返回错误。
    uint32_t open_channel(const std::string& kind, bool ordered, std::string* error);

    // 按 kind 发送（自动查同名通道；通道不存在或未连通时返回 false）。
    bool send(const std::string& kind, const void* data, size_t len, std::string* error);
    bool send(uint32_t channel, const void* data, size_t len, std::string* error);

    void close_channel(const std::string& kind);
    void close_channel(uint32_t channel);

    // 阻塞等首个连接建立；已连通时立即返回 true，超时返回 false。
    bool wait_connected(int timeout_ms);
    bool connected() const;

    // 已打开的 kind 名（服务端为对端已建立的通道）。
    std::vector<std::string> open_kinds() const;
    NetTunnelStats stats() const;
    std::string stats_json() const;

    // 停止并回收（幂等；可在任何线程调用，析构也会调用）。
    void close();

private:
    NetTunnel() = default;
    bool start_dialer(std::string* error);
    bool start_server(std::string* error);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sdk
}  // namespace kop
