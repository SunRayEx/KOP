// KOPNET 协议层：RTP / RTCP / STUN 头部解析。
//
// 远程控制中的实时媒体（KOPMS 远程桌面画面/输入）经 RTP 类协议传输时，
// 需要识别报文类别与序列号。本模块只做“头部识别”，不依赖外部媒体栈：
// RTC adapter 在单端口上复用 STUN/RTP/RTCP 时靠 classify_media_packet
// 分流，隧道把 RTP 报文整包当成一帧载荷。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace kopnet {

// STUN magic cookie（RFC 5389）
#define KOPNET_STUN_MAGIC UINT32_C(0x2112A442)

// 报文类别（单端口复用分流用）
enum class MediaPacketKind {
    Rtp,
    Rtcp,
    Stun,
    Other,  // DTLS/SCTP 或无法识别
};

struct RtpHeader {
    uint8_t version = 0;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    uint16_t extension_id = 0;
    size_t extension_bytes = 0;
};

struct RtcpHeader {
    uint8_t version = 0;
    bool padding = false;
    uint8_t count = 0;
    uint8_t payload_type = 0;
    uint16_t length_words = 0;  // 不含头部的 32 位字数
    uint32_t ssrc = 0;
};

struct StunHeader {
    uint16_t type = 0;
    uint16_t length = 0;  // 不含 20 字节头部
    uint32_t magic = 0;
    uint8_t transaction_id[12] = {0};
};

// 解析 RTP 头部。成功时 payload_offset 指向负载起点（已跳过 CSRC 与扩展）。
bool parse_rtp(const uint8_t* data, size_t len, RtpHeader* out, size_t* payload_offset,
               std::string* error);

// 解析 RTCP 公共头部。
bool parse_rtcp(const uint8_t* data, size_t len, RtcpHeader* out, std::string* error);

// 解析 STUN 头部（要求 magic cookie 匹配）。
bool parse_stun(const uint8_t* data, size_t len, StunHeader* out, std::string* error);

// 单端口复用分流。依据 RFC 7985/WebRTC 惯例：
//   首字节高 2 位 == 0 且 offset4 为 STUN magic → Stun
//   首字节高 2 位 == 2 且 PT 落在 [64,95] → Rtcp
//   首字节高 2 位 == 2 且 PT 为合法 RTP PT → Rtp
//   其余 → Other（DTLS/SCTP 等交给上层）
MediaPacketKind classify_media_packet(const uint8_t* data, size_t len);

// RTCP 负载类型集合
bool is_rtcp_payload_type(uint8_t pt);

}  // namespace kopnet
