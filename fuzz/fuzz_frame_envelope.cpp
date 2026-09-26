// 模糊靶：KOPNET 帧信封反序列化（网络侧收到的 KOPAW 帧）。
//
// 这是“把不可信字节变成 KopawFrame”的地方：平面计数、宽高、stride、
// 载荷偏移都来自 wire。任何越界都会直接变成内存安全问题。
#include "fuzz_driver.hpp"

#include "frame.hpp"
#include "frame_envelope.hpp"

#include <string>
#include <vector>

namespace {

// 序列化一条合法的 CPU 视频（RGBA）帧作为种子：变异会改坏它的各种长度域，
// 从而覆盖“平面计数超限 / stride 不足 / 载荷截断”等失败路径。
std::vector<uint8_t> make_frame_seed() {
    const uint32_t w = 16;
    const uint32_t h = 8;
    const std::vector<uint8_t> payload(w * h * 4, 0xAB);
    kopaw::OwnedFrame* o =
        kopaw::make_frame(KOPAW_MEDIA_VIDEO, 1000, 1000, payload.size());
    std::memcpy(o->data(), payload.data(), payload.size());
    o->frame.format.video.width = w;
    o->frame.format.video.height = h;
    o->frame.stride = w * 4;
    o->frame.color.range = KOPAW_COLOR_RANGE_LIMITED;
    o->frame.color.matrix = KOPAW_COLOR_MATRIX_BT709;

    std::vector<uint8_t> wire;
    uint32_t plane_fd_count = 0;
    bool send_fence = false;
    std::string error;
    if (!kopnet::serialize_frame(o->frame, &wire, &plane_fd_count, &send_fence, &error)) {
        return std::vector<uint8_t>{0};
    }
    o->frame.release(&o->frame);  // 归还池化帧，避免本靶自身泄漏
    return wire;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len) {
    KopawFrame out_frame{};
    out_frame.struct_size = sizeof(KopawFrame);
    std::vector<uint8_t> payload;
    std::string error;
    // fuzz 靶不提供 fd：CPU 帧路径不依赖 fd；DMA 路径会在 fd 校验处失败，
    // 这正是我们想反复变异的失败分支之一。
    if (kopnet::deserialize_frame(data, len, nullptr, 0, &out_frame, &payload, &error)) {
        // 成功路径的不变式：载荷缓冲必须能装下 frame.size。
        if (out_frame.size > 0 && payload.size() < out_frame.size) return 1;
        // 反序列化成功的帧必须由 release 归还，避免本靶自身泄漏。
        if (out_frame.release) out_frame.release(&out_frame);
    }
    return 0;
}

extern "C" void kop_fuzz_make_seeds(kop_fuzz::Seeds* out) {
    out->buffers.push_back(make_frame_seed());
    out->buffers.emplace_back();
    // 头部刚好等长的输入：边界条件。
    out->buffers.push_back(std::vector<uint8_t>(kopnet::kFrameEnvelopeHeaderSize, 0));
}
