// KOPNET 帧信封序列化/反序列化实现。
#include "frame_envelope.hpp"

#include <cstring>

#include "frame.hpp"
#include "kopnet/tunnel.hpp"

namespace kopnet {

namespace {

class Writer {
public:
    explicit Writer(std::vector<uint8_t>* out)
        : out_(out), p_(nullptr), n_(0) {}

    void reset(size_t total) {
        out_->clear();
        out_->resize(total);
        p_ = out_->data();
        n_ = 0;
    }

    void u8(uint8_t v) { p_[n_++] = v; }
    void bytes(const uint8_t* b, size_t len) {
        if (len > 0) std::memcpy(p_ + n_, b, len);
        n_ += len;
    }
    void u16(uint16_t v) {
        p_[n_++] = static_cast<uint8_t>(v);
        p_[n_++] = static_cast<uint8_t>(v >> 8);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            p_[n_++] = static_cast<uint8_t>(v >> (8 * i));
        }
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            p_[n_++] = static_cast<uint8_t>(v >> (8 * i));
        }
    }

private:
    std::vector<uint8_t>* out_;
    uint8_t* p_;
    size_t n_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    bool can_read(size_t need) const {
        return static_cast<size_t>(end_ - p_) >= need;
    }

    uint8_t u8() { return *p_++; }
    uint16_t u16() {
        uint16_t v = static_cast<uint16_t>(p_[0]) |
                     (static_cast<uint16_t>(p_[1]) << 8);
        p_ += 2;
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(p_[i]) << (8 * i);
        p_ += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(p_[i]) << (8 * i);
        p_ += 8;
        return v;
    }
    const uint8_t* cursor() const { return p_; }
    void skip(size_t n) { p_ += n; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace

const KopawColorMetadata kUnknownColor{};

bool serialize_frame(const KopawFrame& frame, std::vector<uint8_t>* wire,
                     uint32_t* plane_fd_count_out, bool* send_fence_fd_out,
                     std::string* error) {
    if (!wire) {
        if (error) *error = "wire 输出参数为空";
        return false;
    }
    const bool dmabuf = frame.memory_type == KOPAW_MEMORY_DMABUF;
    uint32_t planes = dmabuf ? frame.plane_count : 0;
    if (planes > KOPAW_MAX_DMABUF_PLANES) {
        if (error) *error = "plane_count 超过 KOPAW_MAX_DMABUF_PLANES";
        return false;
    }

    const uint8_t* payload = nullptr;
    uint64_t payload_size = 0;
    if (!dmabuf) {
        payload = kopaw::cpu_data(&frame);
        payload_size = payload ? static_cast<uint64_t>(frame.size) : 0;
    }

    const size_t total = kFrameEnvelopeHeaderSize +
                         static_cast<size_t>(planes) * kFrameEnvelopePlaneSize +
                         static_cast<size_t>(payload_size);
    if (total > KOPNET_TUNNEL_MAX_PAYLOAD) {
        if (error) *error = "帧信封超过隧道单帧载荷上限";
        return false;
    }

    uint32_t plane_fd_count = 0;
    uint8_t plane_fd_mask = 0;
    if (dmabuf) {
        for (uint32_t i = 0; i < planes; ++i) {
            if (frame.planes[i].fd >= 0) {
                ++plane_fd_count;
                plane_fd_mask |= static_cast<uint8_t>(1u << i);
            }
        }
    }
    const bool send_fence =
        dmabuf && frame.acquire_fence.kind == KOPAW_SYNC_FENCE_FD &&
        frame.acquire_fence.fd >= 0;

    // 色彩 tail 是 5.3 扩展：旧 struct_size 的帧按未知发送（全 0）
    const size_t color_end =
        offsetof(KopawFrame, color) + sizeof(KopawColorMetadata);
    const bool has_color = frame.struct_size >= color_end;

    Writer w(wire);
    w.reset(total);
    w.u8('K');
    w.u8('O');
    w.u8('P');
    w.u8('F');
    w.u8(kFrameEnvelopeVersion);
    w.u8(static_cast<uint8_t>(dmabuf ? KOPAW_MEMORY_DMABUF : KOPAW_MEMORY_CPU));
    w.u16(static_cast<uint16_t>(frame.flags & 0xFFFFu));
    w.u32(static_cast<uint32_t>(frame.media_type));
    w.u64(static_cast<uint64_t>(frame.pts));
    w.u64(static_cast<uint64_t>(frame.dts));
    w.u32(frame.format.video.width);
    w.u32(frame.format.video.height);
    w.u32(frame.drm_fourcc);
    w.u32(frame.stride);
    w.u8(static_cast<uint8_t>(planes));
    w.u8(static_cast<uint8_t>(frame.acquire_fence.kind));
    w.u8(static_cast<uint8_t>(plane_fd_count));
    w.u8(plane_fd_mask);
    w.u64(payload_size);
    const KopawColorMetadata& c = has_color ? frame.color : kUnknownColor;
    w.u32(c.range);
    w.u32(c.matrix);
    w.u32(c.transfer);
    w.u32(c.primaries);
    w.u32(c.chroma_location);
    w.u32(c.flags);
    const KopawHdrMetadata& h = c.hdr;
    w.u32(h.flags);
    w.u32(h.max_luminance);
    w.u32(h.min_luminance);
    w.u32(h.max_cll);
    w.u32(h.max_fall);
    for (int i = 0; i < 6; ++i) w.u32(h.display_primaries[i]);
    w.u32(h.white_point[0]);
    w.u32(h.white_point[1]);
    for (uint32_t i = 0; i < planes; ++i) {
        const KopawDmabufPlane& pl = frame.planes[i];
        w.u32(pl.offset);
        w.u32(pl.stride);
        w.u64(pl.modifier);
    }
    if (payload_size > 0) w.bytes(payload, static_cast<size_t>(payload_size));

    if (plane_fd_count_out) *plane_fd_count_out = plane_fd_count;
    if (send_fence_fd_out) *send_fence_fd_out = send_fence;
    return true;
}

bool deserialize_frame(const uint8_t* data, size_t len, const int* fds,
                       size_t fd_count, KopawFrame* out_frame,
                       std::vector<uint8_t>* payload_out, std::string* error) {
    const auto fail = [&error](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (!data || !out_frame) return fail("deserialize_frame 参数为空");
    if (len < kFrameEnvelopeHeaderSize) return fail("帧信封过短");
    Reader r(data, len);
    if (r.u8() != 'K' || r.u8() != 'O' || r.u8() != 'P' || r.u8() != 'F')
        return fail("帧信封 magic 不匹配");
    if (r.u8() != kFrameEnvelopeVersion) return fail("帧信封版本不支持");

    const uint8_t memory = r.u8();
    const uint16_t flags = r.u16();
    const uint32_t media_type = r.u32();
    const int64_t pts = static_cast<int64_t>(r.u64());
    const int64_t dts = static_cast<int64_t>(r.u64());
    const uint32_t w0 = r.u32();
    const uint32_t h0 = r.u32();
    const uint32_t drm_fourcc = r.u32();
    const uint32_t stride = r.u32();
    const uint8_t plane_count = r.u8();
    const uint8_t fence_kind = r.u8();
    const uint8_t plane_fd_count = r.u8();
    const uint8_t plane_fd_mask = r.u8();
    const uint64_t payload_size = r.u64();
    // color + hdr
    const uint32_t color[6] = {r.u32(), r.u32(), r.u32(),
                               r.u32(), r.u32(), r.u32()};
    const uint32_t hdr_flags = r.u32();
    const uint32_t hdr_max_lum = r.u32();
    const uint32_t hdr_min_lum = r.u32();
    const uint32_t hdr_max_cll = r.u32();
    const uint32_t hdr_max_fall = r.u32();
    uint32_t hdr_primaries[6];
    for (int i = 0; i < 6; ++i) hdr_primaries[i] = r.u32();
    const uint32_t hdr_white[2] = {r.u32(), r.u32()};

    if (plane_count > KOPAW_MAX_DMABUF_PLANES) return fail("plane_count 超限");
    if (plane_fd_count > plane_count)
        return fail("plane_fd_count 超过 plane_count");
    const size_t planes_bytes =
        static_cast<size_t>(plane_count) * kFrameEnvelopePlaneSize;
    if (len != kFrameEnvelopeHeaderSize + planes_bytes +
                   static_cast<size_t>(payload_size))
        return fail("帧信封长度与消息长度不一致");
    const bool want_fence = (fence_kind == KOPAW_SYNC_FENCE_FD);
    const size_t expect_fds = static_cast<size_t>(plane_fd_count) +
                             (want_fence ? 1 : 0);
    if (fd_count != expect_fds) return fail("帧信封 fd 数与消息不符");

    // 填充帧（不动 struct_size/retain/release/user_data，由帧工厂设置）
    KopawFrame& f = *out_frame;
    f.media_type = static_cast<KopawMediaType>(media_type);
    f.flags = flags;
    f.pts = pts;
    f.dts = dts;
    f.size = 0;
    f.stride = stride;
    f.memory_type = memory;
    f.dma_fd = -1;
    f.dma_buf_handle = 0;  // 跨进程句柄无意义，消费方只用 planes[] 的 fd
    f.plane_count = plane_count;
    if (media_type == KOPAW_MEDIA_AUDIO) {
        f.format.audio.sample_rate = w0;
        f.format.audio.channels = h0;
    } else {
        f.format.video.width = w0;
        f.format.video.height = h0;
    }
    f.drm_fourcc = drm_fourcc;
    for (auto& pl : f.planes) {
        pl.fd = -1;
        pl.offset = 0;
        pl.stride = 0;
        pl.modifier = 0;
    }
    f.acquire_fence.kind = fence_kind;
    f.acquire_fence.fd = -1;
    f.acquire_fence.value = 0;
    KopawColorMetadata& c = f.color;
    c.range = color[0];
    c.matrix = color[1];
    c.transfer = color[2];
    c.primaries = color[3];
    c.chroma_location = color[4];
    c.flags = color[5];
    KopawHdrMetadata& hd = c.hdr;
    hd.flags = hdr_flags;
    hd.max_luminance = hdr_max_lum;
    hd.min_luminance = hdr_min_lum;
    hd.max_cll = hdr_max_cll;
    hd.max_fall = hdr_max_fall;
    for (int i = 0; i < 6; ++i) hd.display_primaries[i] = hdr_primaries[i];
    hd.white_point[0] = hdr_white[0];
    hd.white_point[1] = hdr_white[1];
    hd.reserved[0] = 0;

    size_t fd_idx = 0;
    if (memory == KOPAW_MEMORY_DMABUF) {
        for (uint32_t i = 0; i < plane_count; ++i) {
            f.planes[i].offset = r.u32();
            f.planes[i].stride = r.u32();
            f.planes[i].modifier = r.u64();
        }
        for (uint32_t i = 0; i < plane_count; ++i) {
            if ((plane_fd_mask >> i) & 1u) {
                f.planes[i].fd = fds[fd_idx++];
            }
        }
        // 单 fd 的单对象帧：镜像到 dma_fd（与单平面生产端约定一致）
        if (plane_fd_count == 1) f.dma_fd = f.planes[0].fd;
    } else if (payload_size > 0) {
        if (!payload_out) return fail("CPU 帧缺少 payload 输出参数");
        payload_out->resize(static_cast<size_t>(payload_size));
        std::memcpy(payload_out->data(), r.cursor(),
                    static_cast<size_t>(payload_size));
        r.skip(static_cast<size_t>(payload_size));
    }
    if (want_fence) f.acquire_fence.fd = fds[fd_idx++];

    return true;
}

}  // namespace kopnet
