// KOPAW 应用 SDK：把解复用/解码/汇聚节点的装配细节封装成一个 facade。
//
// 典型用法（无窗口、纯数据面）：
//   kop::sdk::PipelineOptions opts;
//   opts.media = "test_media.mkv";
//   opts.on_frame = [](const kop::sdk::FrameView& f) {
//       // f.data/f.width/f.height/f.pts —— 回调返回前帧引用有效
//   };
//   auto pipeline = kop::sdk::Pipeline::open(opts, &err);
//   pipeline->start(&err);
//   pipeline->wait_finished(30000);
//
// 所有权约定与内部节点一致：回调期间帧归管线，返回后由管线释放；
// 需要留存数据请自行拷贝。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "kopaw_abi.h"

namespace kop {
namespace sdk {

struct FrameView {
    int32_t media_type = KOPAW_MEDIA_VIDEO;  // KOPAW_MEDIA_VIDEO / AUDIO
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;   // 视频行距；音频为 0
    int64_t pts = 0;
    int64_t dts = 0;
    bool eos = false;
    const uint8_t* data = nullptr;  // 回调返回前有效
    size_t size = 0;
};

struct PipelineOptions {
    std::string media;            // 媒体文件（本地路径或 RTSP/HTTP URL）
    std::string video_filter;     // lavfi 视频滤镜链（可选，如 "scale=320:240"）
    std::string audio_filter;     // lavfi 音频滤镜链（可选）
    bool audio = false;           // 启用音频播放（PortAudio，SDK 负责 Pa 初始化）
    bool video = true;
    uint32_t timeout_ms = 15000;  // 网络输入单次 I/O 超时
    uint32_t buffer_ms = 250;     // 网络抖动缓冲
    // 视频帧回调（解码线程调用；返回后帧由管线回收）
    std::function<void(const FrameView&)> on_frame;
    // 诊断日志回调（可选；不发则写 stderr）
    std::function<void(const std::string&)> on_log;
};

class Pipeline {
public:
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // 打开媒体并装配管线（解复用/解码/回调汇聚）。失败返回 nullptr 并填 error。
    static std::unique_ptr<Pipeline> open(const PipelineOptions& options,
                                          std::string* error);

    bool start(std::string* error);
    // 请求停止并等待节点线程退出；返回状态（KOPAW_STATE_STOPPING/FINISHED）
    int stop(uint32_t timeout_ms = 2000);
    // 阻塞等待 FINISHED / ERROR；timeout 到期返回当前状态
    // （主动停止请调用 stop()，完成后状态为 STOPPING）
    int wait_finished(uint32_t timeout_ms);
    int state() const;

    // 性能基线（JSON：每节点 delivered/busy、每链路 enqueued/dequeued/max_occ）
    std::string stats_json() const;

private:
    Pipeline() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sdk
}  // namespace kop
