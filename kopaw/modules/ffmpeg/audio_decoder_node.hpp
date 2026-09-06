// 音频解码节点（响应式）：收包帧 → 解码 → swresample → f32/48k/立体声帧发射。
#pragma once
#include <string>

// FFmpeg 8 起公共头不再自带 extern "C" 保护，C++ 使用方必须自行包裹。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include "kopaw_abi.h"

namespace kopaw {

class AudioDecoderNode {
public:
    AudioDecoderNode() = default;
    ~AudioDecoderNode();

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

    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    int out_rate_ = 48000;
    AVChannelLayout out_layout_{};
    int out_channels_ = 2;

    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
