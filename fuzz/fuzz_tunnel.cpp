// 模糊靶：KOPNET 隧道线协议（帧头 + 控制消息解码）。
//
// 隧道头是远程控制链路的第一道解析，控制消息决定通道建立/credit；
// 二者都直接来自不可信对端。
#include "fuzz_driver.hpp"

#include "tunnel_protocol.hpp"

#include <string>
#include <vector>

namespace {

// 一条合法的 KT 数据帧头（20 字节，小端）。
std::vector<uint8_t> make_header_seed() {
    kopnet::TunnelHeader h{};
    h.magic = KOPNET_TUNNEL_MAGIC;
    h.version = KOPNET_TUNNEL_VERSION;
    h.flags = KOPNET_TUNNEL_FLAG_NONE;
    h.channel_id = 1;
    h.reserved = 0;
    h.sequence = 42;
    h.payload_len = 16;
    h.fd_count = 0;
    std::vector<uint8_t> b(KOPNET_TUNNEL_HEADER_SIZE + 16, 0);
    kopnet::encode_tunnel_header(h, b.data());
    return b;
}

// 一条合法的 HELLO 控制帧（payload 部分）。
std::vector<uint8_t> make_hello_seed() {
    kopnet::ControlHello hello{};
    hello.ver_major = KOPNET_TUNNEL_VERSION;
    hello.ver_minor = 0;
    hello.capabilities = KOPNET_TUNNEL_CAP_STREAM_CHANNELS |
                         KOPNET_TUNNEL_CAP_DATAGRAM_CHANNELS;
    hello.max_channels = 8;
    hello.credit = 16;
    return kopnet::encode_control(kopnet::ControlOp::Hello, &hello);
}

std::vector<uint8_t> make_credit_seed() {
    kopnet::ControlOpen open{};
    open.channel_id = 1;
    open.mode = kopnet::ChannelMode::Stream;
    open.kind = "media";
    return kopnet::encode_control(kopnet::ControlOp::Open, &open);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len) {
    std::string error;

    // ---- 帧头 ----
    kopnet::TunnelHeader h{};
    if (kopnet::decode_tunnel_header(data, len, &h)) {
        // decode 不校验 magic（调用方按场景决定是否接受），只验证返回值与
        // 最小长度约束一致：小于头部长度时必须失败。
        if (len < KOPNET_TUNNEL_HEADER_SIZE) return 1;
    }

    // ---- 控制消息（整条输入当作 payload）----
    std::vector<uint8_t> payload(data, data + len);
    kopnet::ControlOp op = static_cast<kopnet::ControlOp>(0);
    kopnet::ControlHello hello{};
    kopnet::ControlHelloAck ack{};
    kopnet::ControlOpen open{};
    kopnet::ControlOpenAck open_ack{};
    uint32_t channel_id = 0;
    uint32_t value = 0;
    (void)kopnet::decode_control(payload, &op, &hello, &ack, &open, &open_ack,
                                 &channel_id, &value, &error);

    // 控制操作码的名称查找必须对任意值安全。
    (void)kopnet::control_op_name(op);
    (void)kopnet::channel_mode_name(kopnet::ChannelMode::Stream);
    return 0;
}

extern "C" void kop_fuzz_make_seeds(kop_fuzz::Seeds* out) {
    out->buffers.push_back(make_header_seed());
    out->buffers.push_back(make_hello_seed());
    out->buffers.push_back(make_credit_seed());
    out->buffers.emplace_back();
}
