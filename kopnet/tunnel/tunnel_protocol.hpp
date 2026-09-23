// KOPNET 隧道线协议（KT, KOPNET Tunnel）。
//
// 这是“透明”的核心：所有远程控制流量都被切分成带通道号的帧。上层只面对
// 逻辑通道，不关心底下是 TCP/UDP/SSH/RDP。帧格式（小端）：
//
//   偏移  长度  字段
//   0     4     magic = 'KPNT' (0x4B504E54)
//   4     1     version
//   5     1     flags      (bit1 = control)
//   6     1     channel_id (0 = 控制通道)
//   7     1     reserved
//   8     4     sequence   （每通道单调；数据帧携带，控制帧为 0）
//   12    4     payload_len
//   16    4     fd_count   （仅信息量；fd 由 stream 传输经 SCM_RIGHTS 随行）
//   20    ...   payload
//
// 可靠性策略：stream 语义传输（tcp/unix/ssh/rdp）本身可靠有序，隧道只负责
// 分帧 + 每通道 credit 回压；datagram 语义传输（udp/rtp/rtc）下通道只能
// 选择 Datagram 模式，乱序/丢包由上层（如 RTP 序列号）处理。选择
// Stream 模式却落在 datagram 传输上时，适配器层直接拒绝并报错，
// 不伪造可靠性。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wire.hpp"

#include "kopnet/tunnel.hpp"  // ChannelMode

namespace kopnet {

#define KOPNET_TUNNEL_MAGIC UINT32_C(0x4B504E54)
#define KOPNET_TUNNEL_HEADER_SIZE 20
#define KOPNET_TUNNEL_VERSION 1
// 通道标识字符串上限（如 "media" / "control" / "input"）
#define KOPNET_TUNNEL_KIND_MAX 16

// 控制通道 id
#define KOPNET_CONTROL_CHANNEL 0

// 帧标志
#define KOPNET_TUNNEL_FLAG_NONE 0
#define KOPNET_TUNNEL_FLAG_CONTROL (1 << 1)

// 控制操作码
enum class ControlOp : uint32_t {
    Hello = 1,
    HelloAck = 2,
    Open = 3,
    OpenAck = 4,
    Close = 5,
    Credit = 6,
    Ping = 7,
    Pong = 8,
    Goodbye = 9,
};

#define KOPNET_TUNNEL_CAP_DATAGRAM_CHANNELS UINT64_C(1)
#define KOPNET_TUNNEL_CAP_STREAM_CHANNELS UINT64_C(2)
#define KOPNET_TUNNEL_CAP_FD_PASSING UINT64_C(4)

// 控制消息的 host 视图
struct ControlHello {
    uint16_t ver_major = KOPNET_TUNNEL_VERSION;
    uint16_t ver_minor = 0;
    uint64_t capabilities = 0;
    uint32_t max_channels = KOPNET_TUNNEL_MAX_CHANNELS;
    uint32_t credit = KOPNET_TUNNEL_DEFAULT_CREDIT;
    uint32_t max_frame = KOPNET_TUNNEL_MAX_FRAME;
};

struct ControlHelloAck {
    uint32_t status = 0;  // 0 = ok
    uint16_t sel_major = KOPNET_TUNNEL_VERSION;
    uint16_t sel_minor = 0;
    uint64_t capabilities = 0;
    uint32_t max_channels = KOPNET_TUNNEL_MAX_CHANNELS;
    uint32_t credit = KOPNET_TUNNEL_DEFAULT_CREDIT;
    uint32_t max_frame = KOPNET_TUNNEL_MAX_FRAME;
};

struct ControlOpen {
    uint32_t channel_id = 0;
    ChannelMode mode = ChannelMode::Datagram;
    std::string kind;
};

struct ControlOpenAck {
    uint32_t channel_id = 0;
    uint32_t status = 0;  // 0 = ok, 1 = refused
    ChannelMode mode = ChannelMode::Datagram;
};

struct ControlCredit {
    uint32_t channel_id = 0;
    uint32_t credit = 0;
};

// 帧头部编码/解码
struct TunnelHeader {
    uint32_t magic = 0;
    uint8_t version = 0;
    uint8_t flags = 0;
    uint8_t channel_id = 0;
    uint8_t reserved = 0;
    uint32_t sequence = 0;
    uint32_t payload_len = 0;
    uint32_t fd_count = 0;
};

void encode_tunnel_header(const TunnelHeader& h, uint8_t* out);
bool decode_tunnel_header(const uint8_t* data, size_t size, TunnelHeader* h);

// 控制消息编解码（payload 部分）
std::vector<uint8_t> encode_control(ControlOp op, const void* msg);
bool decode_control(const std::vector<uint8_t>& payload, ControlOp* op,
                    ControlHello* hello, ControlHelloAck* ack, ControlOpen* open,
                    ControlOpenAck* open_ack, uint32_t* channel_id, uint32_t* value,
                    std::string* error);

// 控制帧 payload 的 op 编解码（HELLO 之外的命令通用前缀）。
inline std::vector<uint8_t> wrap_control(ControlOp op, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    uint32_t opv = static_cast<uint32_t>(op);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((opv >> (8 * i)) & 0xff));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const char* control_op_name(ControlOp op);
const char* channel_mode_name(ChannelMode m);

}  // namespace kopnet
