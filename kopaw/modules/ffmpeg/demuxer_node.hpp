// 解复用器节点（源节点）：读容器，按流拆分为视频/音频包帧。
#pragma once
#include <atomic>
#include <string>
#include <utility>
#include <vector>

// FFmpeg 8 起公共头不再自带 extern "C" 保护，C++ 使用方必须自行包裹。
// （注意：kopaw_abi.h 自带保护，绝不能再包一层。）
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "kopaw_abi.h"
#include "io_control.hpp"

namespace kopaw {

class DemuxerNode {
public:
    DemuxerNode() = default;
    ~DemuxerNode();

    // 打开文件/URL 并探测流。opts 传给 avformat_open_input（如 rtsp_transport=tcp）。
    // 失败时返回 false 并填充 error。
    bool open(const std::string& path,
              const std::vector<std::pair<std::string, std::string>>& opts,
              std::string* error);

    // 网络输入默认有有限的阻塞上限；timeout_ms=0 表示仅响应显式 stop。
    // buffer_ms 映射到 FFmpeg 的 max_delay，用于控制网络抖动缓冲。
    void set_io_policy(uint32_t timeout_ms, uint32_t buffer_ms) {
        timeout_ms_ = timeout_ms;
        buffer_ms_ = buffer_ms;
    }

    // 解码器构建所需的流参数（DemuxerNode 存活期间有效；
    // 解码器仅在构造时拷贝这些参数，之后不保留引用）。
    AVCodecParameters* video_codec_params() const {
        return video_stream_ >= 0 ? fmt_->streams[video_stream_]->codecpar : nullptr;
    }
    AVCodecParameters* audio_codec_params() const {
        return audio_stream_ >= 0 ? fmt_->streams[audio_stream_]->codecpar : nullptr;
    }
    bool has_video() const { return video_stream_ >= 0; }
    bool has_audio() const { return audio_stream_ >= 0; }

    // 图注册信息；注册后由 player 调 set_outputs 回填输出句柄
    KopawNodeDesc desc(KopawGraph* g);
    void set_outputs(KopawOutput video_out, KopawOutput audio_out) {
        video_out_ = video_out;
        audio_out_ = audio_out;
    }

    // 引擎线程入口（vtable.run 转发至此）
    int32_t run_impl();

    // vtable.stop 转发：请求源循环尽快退出
    void request_stop() { stopped_.store(true, std::memory_order_relaxed); }

private:
    void emit_packet(AVPacket* pkt, AVRational tb, KopawOutput out, int32_t media_type);
    void emit_eos(KopawOutput out, int32_t media_type);
    static bool is_network_url(const std::string& path);
    static bool has_option(const std::vector<std::pair<std::string, std::string>>& opts,
                           const char* key);

    AVFormatContext* fmt_ = nullptr;
    int video_stream_ = -1;
    int audio_stream_ = -1;
    KopawGraph* g_ = nullptr;
    KopawOutput video_out_{};
    KopawOutput audio_out_{};
    std::atomic<bool> stopped_{false};
    FfmpegIoControl io_{&stopped_};
    uint32_t timeout_ms_ = 15000;
    uint32_t buffer_ms_ = 250;
};

}  // namespace kopaw
