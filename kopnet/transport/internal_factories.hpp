// KOPNET 传输内部工厂声明（每个传输实现文件提供一对 connect/listen）。
// 仅供 kopnet 内部使用。
#pragma once

#include <memory>
#include <string>

#include "kopnet/transport.hpp"
#include "kopnet/transport_listener.hpp"

namespace kopnet {

// AF_UNIX
std::unique_ptr<Transport> unix_connect(const std::string& name_or_path,
                                        std::string* error);
std::unique_ptr<TransportListener> unix_listen(const std::string& name_or_path,
                                               std::string* error);

// TCP
std::unique_ptr<Transport> tcp_connect(const std::string& host, uint16_t port,
                                       std::string* error);
std::unique_ptr<TransportListener> tcp_listen(const std::string& host, uint16_t port,
                                              std::string* error);

// UDP（connect 语义：绑定本地端口并连接对端；serve 监听首个对端）
std::unique_ptr<Transport> udp_connect(const std::string& host, uint16_t port,
                                       std::string* error);
std::unique_ptr<TransportListener> udp_listen(const std::string& host, uint16_t port,
                                              std::string* error);

// 按 scheme 打开“原始”传输（tcp/udp/unix 直连）。复杂 scheme（ssh/rtp/rtc/rdp）
// 由各自 ProtocolAdapter 处理，不经此入口。
std::unique_ptr<Transport> open_raw_transport(const Endpoint& ep, std::string* error);

// 子进程管道（SSH adapter 使用）。
std::unique_ptr<Transport> spawn_pipe_process(const std::vector<std::string>& argv,
                                              std::string* error);

// 忽略 SIGPIPE（写已关闭的管道/socket 不应殺死本进程）。实现见 transport_factory.cpp。
void ignore_sigpipe();


}  // namespace kopnet
