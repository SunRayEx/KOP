// KOPNET：传输监听器（Serve 角色）。
//
// 监听端点并产出已建立的 Transport。语义取自 scheme：
// - 流式（tcp/unix）：每次 accept 产出一条新连接；
// - 数据报（udp）：阻塞到首个对端报文到达，把 socket connect 到该对端
//   后返回（“会话化”的 UDP），后续 accept 由上层按需重新监听。
#pragma once

#include <memory>
#include <string>

#include "kopnet/transport.hpp"

namespace kopnet {

class TransportListener {
public:
    virtual ~TransportListener() = default;
    virtual bool valid() const = 0;
    // 阻塞接受一条连接（timeout_ms<0 = 无限）。无连接时返回 WouldBlock。
    virtual IoStatus accept(std::unique_ptr<Transport>* out, int timeout_ms,
                            std::string* error) = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;
};

// 按端点创建监听器。仅支持 scheme_supports_serve() 为真的 scheme。
std::unique_ptr<TransportListener> create_transport_listener(const Endpoint& ep,
                                                              std::string* error);

}  // namespace kopnet
