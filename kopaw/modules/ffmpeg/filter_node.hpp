// 音视频过滤器节点（P2.4）：libavfilter 桥接。
// 视频使用 RGBA，音频使用 f32 交错；两者均保持微秒时间基。
#pragma once
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include "kopaw_abi.h"

namespace kopaw {

class VideoFilterNode {
public:
    VideoFilterNode() = default;
    ~VideoFilterNode();

    // graph_desc 为 lavfi 滤镜图（单输入单输出，如 "scale=640:360"）。
    bool open(const std::string& graph_desc, std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    void set_output(KopawOutput out) { out_ = out; }
    void set_graph(KopawGraph* g) { g_ = g; }

    int32_t send_impl(KopawFrame* f);

private:
    int32_t configure(const KopawFrame* f);
    int32_t drain_sink(int64_t default_pts);
    int32_t flush();

    AVFilterGraph* fg_ = nullptr;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    bool configured_ = false;
    bool flushed_ = false;
    std::string graph_desc_;

    AVFrame* in_frm_ = nullptr;   // 每次送入前复制到 FFmpeg 自有缓冲
    AVFrame* out_frm_ = nullptr;  // buffersink 输出缓冲

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

class AudioFilterNode {
public:
    AudioFilterNode() = default;
    ~AudioFilterNode();

    // graph_desc 为单输入单输出滤镜链，如 "volume=0.5,aresample=48000"。
    // 输出固定为 f32 交错、48 kHz、立体声，以匹配 AudioSinkNode。
    bool open(const std::string& graph_desc, std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    void set_output(KopawOutput out) { out_ = out; }
    void set_graph(KopawGraph* g) { g_ = g; }

    int32_t send_impl(KopawFrame* f);

private:
    int32_t configure(const KopawFrame* f);
    int32_t drain_sink(int64_t default_pts);
    int32_t flush();

    AVFilterGraph* fg_ = nullptr;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    bool configured_ = false;
    bool flushed_ = false;
    std::string graph_desc_;

    AVFrame* in_frm_ = nullptr;
    AVFrame* out_frm_ = nullptr;

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
