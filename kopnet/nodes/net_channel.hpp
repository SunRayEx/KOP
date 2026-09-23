// 节点侧的通道抽象：把“裸隧道会话”和“带自动重连的会话”统一成一组操作。
//
// NetSourceNode / NetSinkNode 只调用这个接口，不关心底下是单条 Transport
// 还是 ResilientSession（断线自动重连 + Endpoint 列表故障转移）。两种实现：
//   PlainChannel     —— 一次拨号，传输断了会话即死（旧行为）
//   ResilientChannel —— 断线自动重拨/故障转移，逻辑通道跨重连保持
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "kopnet/tunnel.hpp"  // ChannelMode / SendStatus / RecvStatus

namespace kopaw {

class NetChannel {
public:
    virtual ~NetChannel() = default;
    virtual kopnet::SendStatus send(const uint8_t* data, size_t len,
                                    const std::vector<int>* fds, int timeout_ms) = 0;
    virtual kopnet::RecvStatus recv(std::vector<uint8_t>* data, std::vector<int>* fds,
                                    int timeout_ms) = 0;
    // 会话是否仍在工作：裸会话=传输存活；重连会话=重连线程尚未退出
    virtual bool alive() const = 0;
    virtual bool supports_fds() const = 0;
    virtual void stop() = 0;
};

}  // namespace kopaw
