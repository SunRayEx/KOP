// 音视频过滤器节点（P2.4）：libavfilter 桥接。
// 视频使用 RGBA，音频使用 f32 交错；两者均保持微秒时间基。
#pragma once
#include <string>

// FFmpeg 统一入口 + 跨版本声道布局抽象（libav* 只经此两处引用）。
#include "ffmpeg.hpp"
#include "ffmpeg_compat.hpp"
#include "ffmpeg_raii.hpp"  // AvFilterGraph / AvFrame

#include "kopaw_abi.h"

namespace kopaw {

class VideoFilterNode {
public:
    VideoFilterNode() = default;
    // fg_/in_frm_/out_frm_ 由 RAII 持有，默认析构正确释放。
    ~VideoFilterNode() = default;

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

    AvFilterGraph fg_;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    bool configured_ = false;
    bool flushed_ = false;
    bool opened_ = false;
    std::string graph_desc_;

    AvFrame in_frm_;   // 每次送入前复制到 FFmpeg 自有缓冲
    AvFrame out_frm_;  // buffersink 输出缓冲
    // RGBA filter graphs do not expose KOPAW's ABI tail. Preserve the latest
    // stream color contract across the CPU-only path; native YUV bypasses
    // filters altogether.
    KopawColorMetadata color_{};

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

class AudioFilterNode {
public:
    AudioFilterNode() = default;
    ~AudioFilterNode() = default;

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

    AvFilterGraph fg_;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    bool configured_ = false;
    bool flushed_ = false;
    bool opened_ = false;
    std::string graph_desc_;

    AvFrame in_frm_;
    AvFrame out_frm_;

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
