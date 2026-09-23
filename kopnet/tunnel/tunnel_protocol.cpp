// KOPNET 隧道线协议实现。
#include "tunnel_protocol.hpp"

#include <cstring>

namespace kopnet {

void encode_tunnel_header(const TunnelHeader& h, uint8_t* out) {
    uint32_t magic = h.magic;
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>((magic >> (8 * i)) & 0xff);
    out[4] = h.version;
    out[5] = h.flags;
    out[6] = h.channel_id;
    out[7] = h.reserved;
    uint32_t seq = h.sequence;
    for (int i = 0; i < 4; ++i)
        out[8 + i] = static_cast<uint8_t>((seq >> (8 * i)) & 0xff);
    uint32_t plen = h.payload_len;
    for (int i = 0; i < 4; ++i)
        out[12 + i] = static_cast<uint8_t>((plen >> (8 * i)) & 0xff);
    uint32_t fdc = h.fd_count;
    for (int i = 0; i < 4; ++i)
        out[16 + i] = static_cast<uint8_t>((fdc >> (8 * i)) & 0xff);
}

bool decode_tunnel_header(const uint8_t* data, size_t size, TunnelHeader* h) {
    if (size < KOPNET_TUNNEL_HEADER_SIZE) return false;
    uint32_t magic = 0;
    for (int i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(data[i]) << (8 * i);
    h->magic = magic;
    h->version = data[4];
    h->flags = data[5];
    h->channel_id = data[6];
    h->reserved = data[7];
    uint32_t seq = 0;
    for (int i = 0; i < 4; ++i) seq |= static_cast<uint32_t>(data[8 + i]) << (8 * i);
    h->sequence = seq;
    uint32_t plen = 0;
    for (int i = 0; i < 4; ++i) plen |= static_cast<uint32_t>(data[12 + i]) << (8 * i);
    h->payload_len = plen;
    uint32_t fdc = 0;
    for (int i = 0; i < 4; ++i) fdc |= static_cast<uint32_t>(data[16 + i]) << (8 * i);
    h->fd_count = fdc;
    return true;
}

namespace {

// ---- HELLO：struct_size(4) ver_major(2) ver_minor(2) caps(8)
//           max_channels(4) credit(4) max_frame(4) = 28 → pad 32
std::vector<uint8_t> encode_hello(const ControlHello& m) {
    WireWriter w;
    w.u32(32);
    w.u16(m.ver_major);
    w.u16(m.ver_minor);
    w.u64(m.capabilities);
    w.u32(m.max_channels);
    w.u32(m.credit);
    w.u32(m.max_frame);
    w.pad_to(32);
    return w.take();
}

bool decode_hello(WireReader& r, ControlHello* m) {
    uint32_t struct_size = 0;
    if (!r.u32(&struct_size)) return false;
    if (!r.u16(&m->ver_major) || !r.u16(&m->ver_minor) || !r.u64(&m->capabilities) ||
        !r.u32(&m->max_channels) || !r.u32(&m->credit) || !r.u32(&m->max_frame)) {
        return false;
    }
    return true;
}

// ---- HELLO_ACK：struct_size(4) status(4) sel_major(2) sel_minor(2) caps(8)
//                 max_channels(4) credit(4) max_frame(4) = 32
std::vector<uint8_t> encode_hello_ack(const ControlHelloAck& m) {
    WireWriter w;
    w.u32(32);
    w.u32(m.status);
    w.u16(m.sel_major);
    w.u16(m.sel_minor);
    w.u64(m.capabilities);
    w.u32(m.max_channels);
    w.u32(m.credit);
    w.u32(m.max_frame);
    return w.take();
}

bool decode_hello_ack(WireReader& r, ControlHelloAck* m) {
    uint32_t struct_size = 0;
    if (!r.u32(&struct_size)) return false;
    return r.u32(&m->status) && r.u16(&m->sel_major) && r.u16(&m->sel_minor) &&
           r.u64(&m->capabilities) && r.u32(&m->max_channels) && r.u32(&m->credit) &&
           r.u32(&m->max_frame);
}

// ---- OPEN：struct_size(4) channel_id(4) mode(4) kind[16] = 28
std::vector<uint8_t> encode_open(const ControlOpen& m) {
    WireWriter w;
    w.u32(28);
    w.u32(m.channel_id);
    w.u32(static_cast<uint32_t>(m.mode));
    std::string kind = m.kind;
    if (kind.size() > KOPNET_TUNNEL_KIND_MAX) kind.resize(KOPNET_TUNNEL_KIND_MAX);
    std::vector<uint8_t> kb(KOPNET_TUNNEL_KIND_MAX, 0);
    std::memcpy(kb.data(), kind.data(), kind.size());
    w.bytes(kb);
    return w.take();
}

bool decode_open(WireReader& r, ControlOpen* m) {
    uint32_t struct_size = 0;
    if (!r.u32(&struct_size) || !r.u32(&m->channel_id)) return false;
    uint32_t mode = 0;
    if (!r.u32(&mode)) return false;
    m->mode = static_cast<ChannelMode>(mode);
    std::vector<uint8_t> kb;
    if (!r.bytes(&kb, KOPNET_TUNNEL_KIND_MAX)) return false;
    // 去掉 NUL 填充
    size_t len = 0;
    while (len < kb.size() && kb[len] != 0) ++len;
    m->kind.assign(reinterpret_cast<const char*>(kb.data()), len);
    return true;
}

// ---- OPEN_ACK：struct_size(4) channel_id(4) status(4) mode(4) = 16 → pad 20
std::vector<uint8_t> encode_open_ack(const ControlOpenAck& m) {
    WireWriter w;
    w.u32(20);
    w.u32(m.channel_id);
    w.u32(m.status);
    w.u32(static_cast<uint32_t>(m.mode));
    w.pad_to(20);
    return w.take();
}

bool decode_open_ack(WireReader& r, ControlOpenAck* m) {
    uint32_t struct_size = 0;
    if (!r.u32(&struct_size) || !r.u32(&m->channel_id) || !r.u32(&m->status)) return false;
    uint32_t mode = 0;
    if (!r.u32(&mode)) return false;
    m->mode = static_cast<ChannelMode>(mode);
    return true;
}

}  // namespace

std::vector<uint8_t> encode_control(ControlOp op, const void* msg) {
    switch (op) {
        case ControlOp::Hello:
            return encode_hello(*static_cast<const ControlHello*>(msg));
        case ControlOp::HelloAck:
            return encode_hello_ack(*static_cast<const ControlHelloAck*>(msg));
        case ControlOp::Open:
            return encode_open(*static_cast<const ControlOpen*>(msg));
        case ControlOp::OpenAck:
            return encode_open_ack(*static_cast<const ControlOpenAck*>(msg));
        case ControlOp::Close: {
            // channel_id(4) reason(4)
            const ControlOpen* o = static_cast<const ControlOpen*>(msg);
            WireWriter w;
            w.u32(o->channel_id);
            w.u32(0);
            return w.take();
        }
        case ControlOp::Credit: {
            // channel_id(4) credit(4)
            const ControlCredit* o = static_cast<const ControlCredit*>(msg);
            WireWriter w;
            w.u32(o->channel_id);
            w.u32(o->credit);
            return w.take();
        }
        case ControlOp::Ping:
        case ControlOp::Pong: {
            WireWriter w;
            w.u64(0);
            return w.take();
        }
        case ControlOp::Goodbye: {
            WireWriter w;
            w.u32(0);
            return w.take();
        }
        default:
            return {};
    }
}

bool decode_control(const std::vector<uint8_t>& payload, ControlOp* op,
                    ControlHello* hello, ControlHelloAck* ack, ControlOpen* open,
                    ControlOpenAck* open_ack, uint32_t* channel_id, uint32_t* value,
                    std::string* error) {
    // 控制帧 payload = op(4) + body
    if (payload.size() < 4) {
        *error = "控制 payload 过短";
        return false;
    }
    WireReader r(payload);
    uint32_t opv = 0;
    if (!r.u32(&opv)) return false;
    *op = static_cast<ControlOp>(opv);
    switch (*op) {
        case ControlOp::Hello:
            if (hello) {
                if (!decode_hello(r, hello)) {
                    *error = "HELLO 解码失败";
                    return false;
                }
            }
            break;
        case ControlOp::HelloAck:
            if (ack) {
                if (!decode_hello_ack(r, ack)) {
                    *error = "HELLO_ACK 解码失败";
                    return false;
                }
            }
            break;
        case ControlOp::Open:
            if (open) {
                if (!decode_open(r, open)) {
                    *error = "OPEN 解码失败";
                    return false;
                }
            }
            break;
        case ControlOp::OpenAck:
            if (open_ack) {
                if (!decode_open_ack(r, open_ack)) {
                    *error = "OPEN_ACK 解码失败";
                    return false;
                }
            }
            break;
        case ControlOp::Close:
            if (channel_id) {
                uint32_t cid = 0;
                if (!r.u32(&cid)) return false;
                *channel_id = cid;
            }
            break;
        case ControlOp::Credit:
            if (channel_id) {
                uint32_t cid = 0;
                uint32_t credit = 0;
                if (!r.u32(&cid) || !r.u32(&credit)) return false;
                *channel_id = cid;
                if (value) *value = credit;
            }
            break;
        default:
            break;
    }
    return true;
}

const char* control_op_name(ControlOp op) {
    switch (op) {
        case ControlOp::Hello: return "HELLO";
        case ControlOp::HelloAck: return "HELLO_ACK";
        case ControlOp::Open: return "OPEN";
        case ControlOp::OpenAck: return "OPEN_ACK";
        case ControlOp::Close: return "CLOSE";
        case ControlOp::Credit: return "CREDIT";
        case ControlOp::Ping: return "PING";
        case ControlOp::Pong: return "PONG";
        case ControlOp::Goodbye: return "GOODBYE";
        default: return "?";
    }
}

const char* channel_mode_name(ChannelMode m) {
    switch (m) {
        case ChannelMode::Datagram: return "datagram";
        case ChannelMode::Stream: return "stream";
        default: return "?";
    }
}

}  // namespace kopnet
