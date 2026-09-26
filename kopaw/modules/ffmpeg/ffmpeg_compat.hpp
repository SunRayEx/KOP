// FFmpeg 版本兼容层。
//
// 全项目只在此处出现 FFmpeg 版本宏分支，业务代码调用统一入口，因此可以
// 在 CI 矩阵里跨发行版构建（Ubuntu 22.04 = FFmpeg 4.4，24.04 = 6.1/7.0，
// 25.04 = 7.1，发行版 FFmpeg 8）。这是“统一 ABI 调用”的关键收益：
// 换 FFmpeg 版本不改业务代码。
//
// 实际断点只有一个：声道布局 API。
//   libavutil >= 57.28（FFmpeg >= 5.1）：AVChannelLayout + av_channel_layout_*
//   更早版本：int64_t channel_layout + int channels
// 本层把两者归一为 kopaw::compat::ChannelLayout 与一组自由函数，
// 调用方写一份代码即可。其余被使用的 API（send/receive、packet_alloc、
// filter graph、hwframe、audio_fifo）自 FFmpeg 3.x 起稳定，无需兼容。
#pragma once

#include "ffmpeg.hpp"

namespace kopaw {
namespace compat {

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
// ---- FFmpeg >= 5.1：直接使用 AVChannelLayout ----
using ChannelLayout = ::AVChannelLayout;

inline void channel_layout_default(ChannelLayout* layout, int nb_channels) {
    av_channel_layout_default(layout, nb_channels);
}
inline void channel_layout_uninit(ChannelLayout* layout) {
    av_channel_layout_uninit(layout);
}
inline bool channel_layout_describe(const ChannelLayout* layout, char* buf,
                                    size_t buf_size) {
    return av_channel_layout_describe(layout, buf, buf_size) >= 0;
}
inline int channel_layout_copy(ChannelLayout* dst, const ChannelLayout* src) {
    return av_channel_layout_copy(dst, src);
}
inline int channel_layout_compare(const ChannelLayout* a, const ChannelLayout* b) {
    return av_channel_layout_compare(a, b);
}

// 帧声道布局访问
inline void frame_set_channel_layout(AVFrame* frame, const ChannelLayout* layout) {
    av_channel_layout_copy(&frame->ch_layout, layout);
}
inline void frame_get_channel_layout(const AVFrame* frame, ChannelLayout* out) {
    av_channel_layout_copy(out, &frame->ch_layout);
}
// 编解码上下文输出布局
inline void codec_set_channel_layout(AVCodecContext* ctx, const ChannelLayout* layout) {
    av_channel_layout_copy(&ctx->ch_layout, layout);
}

// 重采样器参数：现代版用 swr_alloc_set_opts2（AVChannelLayout 参数）
inline int swr_set_opts(SwrContext** swr, const ChannelLayout* out_layout,
                        AVSampleFormat out_fmt, int out_rate,
                        const ChannelLayout* in_layout, AVSampleFormat in_fmt,
                        int in_rate) {
    return swr_alloc_set_opts2(swr, out_layout, out_fmt, out_rate, in_layout,
                               in_fmt, in_rate, 0, nullptr);
}
#else
// ---- FFmpeg < 5.1：用 int64_t 掩码仿真同一操作集 ----
struct ChannelLayout {
    uint64_t mask = 0;
    int nb_channels = 0;
};

inline void channel_layout_default(ChannelLayout* layout, int nb_channels) {
    layout->mask = static_cast<uint64_t>(av_get_default_channel_layout(nb_channels));
    layout->nb_channels = nb_channels;
}
inline void channel_layout_uninit(ChannelLayout* layout) {
    layout->mask = 0;
    layout->nb_channels = 0;
}
inline bool channel_layout_describe(const ChannelLayout* layout, char* buf,
                                    size_t buf_size) {
    av_get_channel_layout_string(buf, static_cast<int>(buf_size),
                                 layout->nb_channels, layout->mask);
    return buf[0] != '\0';
}
inline int channel_layout_copy(ChannelLayout* dst, const ChannelLayout* src) {
    *dst = *src;
    return 0;
}
inline int channel_layout_compare(const ChannelLayout* a, const ChannelLayout* b) {
    if (a->mask != b->mask) return 1;
    if (a->nb_channels != b->nb_channels) return 1;
    return 0;
}

inline void frame_set_channel_layout(AVFrame* frame, const ChannelLayout* layout) {
    frame->channel_layout = layout->mask;
    frame->channels = layout->nb_channels;
}
inline void frame_get_channel_layout(const AVFrame* frame, ChannelLayout* out) {
    out->mask = static_cast<uint64_t>(frame->channel_layout);
    out->nb_channels = frame->channels;
}
inline void codec_set_channel_layout(AVCodecContext* ctx, const ChannelLayout* layout) {
    ctx->channel_layout = layout->mask;
    ctx->channels = layout->nb_channels;
}

// 旧版用 swr_alloc_set_opts（int64_t 参数）
inline int swr_set_opts(SwrContext** swr, const ChannelLayout* out_layout,
                        AVSampleFormat out_fmt, int out_rate,
                        const ChannelLayout* in_layout, AVSampleFormat in_fmt,
                        int in_rate) {
    SwrContext* s = swr_alloc_set_opts(*swr, out_layout->mask, out_fmt, out_rate,
                                       in_layout->mask, in_fmt, in_rate, 0, nullptr);
    if (!s) return AVERROR(ENOMEM);
    *swr = s;
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// 编解码能力查询：FFmpeg 8 弃用了 AVCodec::pix_fmts / sample_fmts /
// supported_samplerates，改用 avcodec_get_supported_config。返回数组仍以
// AV_PIX_FMT_NONE / AV_SAMPLE_FMT_NONE / 0 结尾（见 avcodec.h 的
// AV_CODEC_CONFIG 文档），调用方迭代方式与旧字段完全一致。
// ---------------------------------------------------------------------------
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 0, 100)

inline const AVPixelFormat* codec_pix_fmts(const AVCodec* codec) {
    const void* out = nullptr;
    int num = 0;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &out,
                                 &num);
    return static_cast<const AVPixelFormat*>(out);
}
inline const AVSampleFormat* codec_sample_fmts(const AVCodec* codec) {
    const void* out = nullptr;
    int num = 0;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &out,
                                 &num);
    return static_cast<const AVSampleFormat*>(out);
}
inline const int* codec_sample_rates(const AVCodec* codec) {
    const void* out = nullptr;
    int num = 0;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &out,
                                 &num);
    return static_cast<const int*>(out);
}
inline const ChannelLayout* codec_channel_layouts(const AVCodec* codec) {
    const void* out = nullptr;
    int num = 0;
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &out,
                                 &num);
    return static_cast<const ChannelLayout*>(out);
}

#else

inline const AVPixelFormat* codec_pix_fmts(const AVCodec* codec) {
    return codec->pix_fmts;
}
inline const AVSampleFormat* codec_sample_fmts(const AVCodec* codec) {
    return codec->sample_fmts;
}
inline const int* codec_sample_rates(const AVCodec* codec) {
    return codec->supported_samplerates;
}
// FFmpeg < 5.1 没有 AVChannelLayout 生态：此分支仅保证头可编译，返回
// nullptr 表示“无约束”。依赖 ch_layout 的调用方（kopaw-transcode）本身
// 要求 FFmpeg >= 5.1；节点库的其余部分不使用该能力查询。
inline const ChannelLayout* codec_channel_layouts(const AVCodec*) { return nullptr; }

#endif

}  // namespace compat
}  // namespace kopaw
