// 音频解码节点（响应式）：收包帧 → 解码 → swresample → f32/48k/立体声帧发射。
#pragma once
#include <string>

#include "ffmpeg.hpp"          // 统一 libav* 入口（extern "C" 只在那里包裹一次）
#include "ffmpeg_compat.hpp"   // 声道布局跨版本抽象
#include "ffmpeg_raii.hpp"     // AVCodecContext/AVPacket/AVFrame/Swr 生命周期

#include "kopaw_abi.h"

namespace kopaw {

class AudioDecoderNode {
public:
    AudioDecoderNode() = default;
    // 全部 FFmpeg 资源由 RAII 句柄持有，默认析构即可正确释放。
    ~AudioDecoderNode() = default;

    // out_rate/out_ch 固定 MVP 输出规格：f32 交错
    bool open(AVCodecParameters* params, std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    void set_output(KopawOutput out) { out_ = out; }
    void set_graph(KopawGraph* g) { g_ = g; }

    int32_t send_impl(KopawFrame* f);

private:
    bool ensure_resampler(const AVFrame* frame);
    int32_t emit_resampled(AVFrame* frame);
    void flush_and_finish();

    AvCodecContext ctx_;
    AvSwr swr_;
    int out_rate_ = 48000;
    compat::ChannelLayout out_layout_{};
    int out_channels_ = 2;

    AvPacket pkt_;
    AvFrame frm_;

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
