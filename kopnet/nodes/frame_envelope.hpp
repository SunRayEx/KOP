// KOPNET 帧信封：KopawFrame 在隧道通道上的线上格式（v1）。
//
// net-sink 把一帧序列化成「定长头 + 每平面记录 + payload」一条消息，平面 fd
// 与 acquire fence fd 随该消息经 SCM_RIGHTS 透传（仅 Unix 系传输支持，见
// Transport::supports_fds）；net-source 反序列化重建帧，收到的 fd 由帧的
// release 统一关闭（OwnedFrame::close_external_fds）。
//
// 头部小端、逐字段序列化，不依赖宿主结构体布局：
//   off  size  字段
//   0    4     magic "KOPF"
//   4    1     version（=1）
//   5    1     memory（0=CPU, 1=DMABUF）
//   6    2     frame_flags（低 16 位）
//   8    4     media_type
//   12   8     pts
//   20   8     dts
//   28   4     width（视频）/ sample_rate（音频）
//   32   4     height（视频）/ channels（音频）
//   36   4     drm_fourcc
//   40   4     stride
//   44   1     plane_count
//   45   1     fence_kind
//   46   1     plane_fd_count（随消息附带的平面 fd 数）
//   47   1     plane_fd_mask（bit i ⇒ planes[i].fd >= 0，fd 在途中）
//   48   8     payload_size
//   56   24    color: range, matrix, transfer, primaries, chroma_location, flags
//   80   52    hdr: flags, max_luminance, min_luminance, max_cll, max_fall,
//              display_primaries[6], white_point[2]
//   132  16*n  planes: {u32 offset, u32 stride, u64 modifier}
//   头 + 平面记录之后是 payload_size 字节载荷（CPU 帧才有）
//
// 一条消息附带的 fd 顺序：先 plane_fd_count 个平面 fd（按 plane 顺序，跳过
// fd<0 的平面），随后若 fence_kind==KOPAW_SYNC_FENCE_FD 则附 1 个 fence fd。
// 同一 DRM 对象的多平面帧会收到多个 fd（各指向同一对象），由 release 逐个关闭。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kopaw_abi.h"

namespace kopnet {

constexpr uint8_t kFrameEnvelopeVersion = 1;
constexpr size_t kFrameEnvelopeHeaderSize = 132;
constexpr size_t kFrameEnvelopePlaneSize = 16;

// 把一帧序列化到 wire（覆盖 wire 全部内容：头 + 平面记录 + 载荷）。
// plane_fd_count_out：需要随消息附带的平面 fd 数（frame 中 fd>=0 的平面数，
// 调用方负责把这些 fd dup 后放入发送队列，顺序按 plane 序）。
// send_fence_fd_out：是否需要再在末尾附一个 acquire fence fd。
// 失败（plane_count 超限、总长超过隧道载荷上限）时返回 false 并置 error。
bool serialize_frame(const KopawFrame& frame, std::vector<uint8_t>* wire,
                     uint32_t* plane_fd_count_out, bool* send_fence_fd_out,
                     std::string* error);

// 反序列化一条消息。fds 是随消息收到的 fd（顺序见上），所有权转移到
// out_frame（由其 release 关闭）。CPU 帧的载荷写入 payload_out。
// 失败时返回 false 并置 error，调用方负责关闭 fds。
bool deserialize_frame(const uint8_t* data, size_t len, const int* fds,
                       size_t fd_count, KopawFrame* out_frame,
                       std::vector<uint8_t>* payload_out, std::string* error);

}  // namespace kopnet
