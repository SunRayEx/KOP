// 硬件解码探测：KOPAW_HWACCEL 环境变量控制（auto|none|vaapi|cuda，默认 auto）。
// 探测顺序（auto）：vaapi → cuda。任一后端就绪即采用；全部失败回退软解。
// 输出帧仍是系统内存 NV12（av_hwframe_transfer_data 拉回），
// 零拷贝 DMA-BUF 直通属 P2 范畴（帧 ABI 已就绪）。
#pragma once
#include <string>

#include "ffmpeg.hpp"
#include "ffmpeg_raii.hpp"  // AvBufferRef

namespace kopaw {

struct HwAccelConfig {
    bool active = false;
    std::string name;              // "vaapi" / "cuda"
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    AvBufferRef device_ref;        // 成功时持有；RAII 释放，无需手工 av_buffer_unref
};

// 按环境变量探测指定编解码器可用的硬解后端。
// 返回 out->active 表示成功创建 hw device 且解码器支持对应 hw pix fmt。
bool hw_probe(AVCodecID codec_id, HwAccelConfig* out);

}  // namespace kopaw
