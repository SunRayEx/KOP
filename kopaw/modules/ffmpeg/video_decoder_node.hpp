// 视频解码节点（响应式）：收包帧 → 解码（硬解探测/回退软解）→
// swscale → 池化 RGBA 帧发射。
#pragma once
#include <string>

// FFmpeg 8 起公共头不再自带 extern "C" 保护，C++ 使用方必须自行包裹。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include "kopaw_abi.h"
#include "hwaccel.hpp"

namespace kopaw {

class VideoDecoderNode {
public:
    VideoDecoderNode() = default;
    ~VideoDecoderNode();

    // params 来自 DemuxerNode（仅构造期使用）；打开解码器失败返回 false。
    bool open(AVCodecParameters* params, std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    void set_output(KopawOutput out) { out_ = out; }
    void set_graph(KopawGraph* g) { g_ = g; }

    int32_t send_impl(KopawFrame* f);

private:
    static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                            const enum AVPixelFormat* fmts);

    bool open_context(AVCodecParameters* params, bool hw, std::string* error);
    void close_context();
    int32_t emit_converted(AVFrame* frame);       // 返回 emit 结果码
    int32_t decode_frame_to_rgba(AVFrame* raw);   // 硬解帧拉回系统内存再转换
    void flush_and_finish();

    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    int sws_src_w_ = 0, sws_src_h_ = 0;
    int sws_src_fmt_ = -1;
    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;

    // 硬解状态
    HwAccelConfig hw_;
    bool hw_active_ = false;

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
