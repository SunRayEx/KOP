// KOPNET 透明网络架构：传输层（Transport）。
//
// Transport 是“一条已经建好的、可读写的数据通路”的抽象。协议适配器
// （SSH/RTC/RTP/RDP/……）负责把各自协议的一条连接包装成 Transport，
// Tunnel 只面向 Transport 编程，因此对协议完全透明。
//
// 两类语义：
// - Stream：可靠有序字节流。Tunnel 自行做长度前缀分帧；read/write 可能
//   返回部分数据（与 recv/send 语义一致），调用方必须循环到整帧完成。
// - Datagram：保留报文边界、可能乱序/丢包。每报文一帧，Tunnel 不做分帧。
//
// FD 透传：Unix 系传输支持 SCM_RIGHTS（supports_fds() 为真），使 DMA-BUF
// fd 能与 KOPMS BUS2LAYER 一致地跨进程透传。对端收到的 fd 由调用方拥有。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kopnet/endpoint.hpp"

namespace kopnet {

// 单帧最多随行的 fd 数（对齐 KOPMS BUS2LAYER 的 planes+1 上限）。
#define KOPNET_MAX_FDS_PER_FRAME 8

// 数据报传输的单包上限（UDP/RTP/RTC；与隧道最大帧一致）。
#define KOPNET_MAX_DATAGRAM 65507

enum class IoStatus {
    Ok,
    WouldBlock,  // 非阻塞模式下无数据/不可写
    Closed,      // 对端正常关闭
    Error,       // 系统错误（详见 error 输出）
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual TransportSemantics semantics() const = 0;
    virtual bool supports_fds() const { return false; }
    virtual bool valid() const = 0;
    virtual void close() = 0;
    virtual std::string describe() const = 0;

    // ---- 流式语义 ----
    // 读取最多 cap 字节到 buf，返回实际读到的字节数。无数据可读时返回
    // WouldBlock。fds（非空时）接收对端经 SCM_RIGHTS 发来的 fd。
    virtual IoStatus read(uint8_t* buf, size_t cap, size_t* n, std::vector<int>* fds,
                          std::string* error) = 0;
    // 写入 buf 的前 len 字节，返回实际写入字节数。不可写时返回 WouldBlock。
    // fds（非空且 transport 支持）随本次写一并发送。
    virtual IoStatus write(const uint8_t* buf, size_t len, size_t* n,
                           const std::vector<int>* fds, std::string* error) = 0;

    // ---- 数据报语义 ----
    virtual IoStatus send_datagram(const uint8_t* data, size_t len,
                                   const std::vector<int>* fds, std::string* error) = 0;
    virtual IoStatus recv_datagram(std::vector<uint8_t>* data, std::vector<int>* fds,
                                   std::string* error) = 0;

    // ---- 事件等待（毫秒；<0 = 无限等待）----
    virtual IoStatus wait_readable(int timeout_ms, std::string* error) = 0;
    virtual IoStatus wait_writable(int timeout_ms, std::string* error) = 0;
};

// AF_UNIX socketpair（用于测试与进程内回环；语义为 Stream，支持 FD 透传）。
// 返回两个已连接的 Transport，分别归两端所有。
bool make_socketpair_transports(std::unique_ptr<Transport>* a, std::unique_ptr<Transport>* b,
                                std::string* error);

// 本进程的 stdin/stdout 接成流式 Transport（kopnet-relay --stdio 用，
// 供 `ssh host kopnet-relay --stdio` 把远端隧道接回本机）。
std::unique_ptr<Transport> make_stdio_transport(std::string* error);

// XDG_RUNTIME_DIR 相对名 → 绝对路径（与 BUS2LAYER 约定一致）。
bool resolve_unix_path(const std::string& name_or_path, std::string* path,
                       std::string* error);

}  // namespace kopnet
