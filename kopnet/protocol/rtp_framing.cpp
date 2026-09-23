// KOPNET RTP/RTCP/STUN 头部解析实现。
#include "rtp_framing.hpp"

namespace kopnet {

namespace {

uint16_t read_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

bool parse_rtp(const uint8_t* data, size_t len, RtpHeader* out, size_t* payload_offset,
               std::string* error) {
    if (len < 12) {
        if (error) *error = "RTP 报文过短";
        return false;
    }
    out->version = (data[0] >> 6) & 0x03;
    out->padding = (data[0] & 0x20) != 0;
    out->extension = (data[0] & 0x10) != 0;
    out->csrc_count = data[0] & 0x0f;
    out->marker = (data[1] & 0x80) != 0;
    out->payload_type = data[1] & 0x7f;
    out->sequence = read_u16_be(data + 2);
    out->timestamp = read_u32_be(data + 4);
    out->ssrc = read_u32_be(data + 8);
    if (out->version != 2) {
        if (error) *error = "RTP 版本不是 2";
        return false;
    }
    size_t off = 12 + 4 * out->csrc_count;
    if (off > len) {
        if (error) *error = "RTP CSRC 列表越界";
        return false;
    }
    if (out->extension) {
        if (off + 4 > len) {
            if (error) *error = "RTP 扩展头越界";
            return false;
        }
        out->extension_id = read_u16_be(data + off);
        out->extension_bytes = 4 * read_u16_be(data + off + 2);
        off += 4 + out->extension_bytes;
        if (off > len) {
            if (error) *error = "RTP 扩展数据越界";
            return false;
        }
    }
    if (payload_offset) *payload_offset = off;
    return true;
}

bool parse_rtcp(const uint8_t* data, size_t len, RtcpHeader* out, std::string* error) {
    if (len < 8) {
        if (error) *error = "RTCP 报文过短";
        return false;
    }
    out->version = (data[0] >> 6) & 0x03;
    out->padding = (data[0] & 0x20) != 0;
    out->count = data[0] & 0x1f;
    out->payload_type = data[1];
    out->length_words = read_u16_be(data + 2);
    out->ssrc = read_u32_be(data + 4);
    if (out->version != 2) {
        if (error) *error = "RTCP 版本不是 2";
        return false;
    }
    return true;
}

bool parse_stun(const uint8_t* data, size_t len, StunHeader* out, std::string* error) {
    if (len < 20) {
        if (error) *error = "STUN 报文过短";
        return false;
    }
    out->type = read_u16_be(data);
    out->length = read_u16_be(data + 2);
    out->magic = read_u32_be(data + 4);
    for (int i = 0; i < 12; ++i) out->transaction_id[i] = data[8 + i];
    if (out->magic != KOPNET_STUN_MAGIC) {
        if (error) *error = "STUN magic cookie 不匹配";
        return false;
    }
    return true;
}

bool is_rtcp_payload_type(uint8_t pt) {
    // RFC 5761：192..223 为 RTCP（其中 64..95 为已知类型）
    return pt >= 192 && pt <= 223;
}

MediaPacketKind classify_media_packet(const uint8_t* data, size_t len) {
    if (len < 4) return MediaPacketKind::Other;
    uint8_t high = (data[0] >> 6) & 0x03;
    if (high == 0 && len >= 8 && read_u32_be(data + 4) == KOPNET_STUN_MAGIC) {
        return MediaPacketKind::Stun;
    }
    if (high == 2) {
        uint8_t pt = data[1] & 0x7f;
        if (is_rtcp_payload_type(pt) || (pt >= 64 && pt <= 95)) return MediaPacketKind::Rtcp;
        // 合法 RTP 动态/静态负载类型：0..63（除保留）与 96..127
        if (pt <= 63 || (pt >= 96 && pt <= 127)) return MediaPacketKind::Rtp;
    }
    return MediaPacketKind::Other;
}

}  // namespace kopnet
