// 音频汇聚节点（响应式 sink）：f32 帧写入 SPSC 环形缓冲，PortAudio 回调消费。
// 音频是 MVP 的主时钟：回调线程按已消费样本数回报媒体时钟。
#pragma once
#include <atomic>
#include <string>

#include "kop/spsc_ring.h"
#include "kopaw_abi.h"
#include "portaudio.h"  // 自带 extern "C" 保护（提供 PaStream 类型）

namespace kopaw {

class AudioSinkNode {
public:
    AudioSinkNode(KopawGraph* g, int rate = 48000, int channels = 2);
    ~AudioSinkNode();

    // 打开默认输出设备。Pa_Initialize 由 player 负责。
    bool open(std::string* error);

    KopawNodeDesc desc();
    void set_output(KopawOutput out) { out_ = out; }  // 仅占位：sink 无输出
    // P1 延迟记账：ring 排空 + 播放完成后由 PA 回调线程调用
    // kopaw_node_sink_done，player 必须在入图后回填节点 id
    void set_node_id(uint32_t id) { node_id_ = id; }
    // 入图后回填图句柄（构造发生在 kopaw_graph_new 之前）
    void set_graph(KopawGraph* g) { g_ = g; }

    int32_t send_impl(KopawFrame* f);
    void stop_impl();

private:
    static int pa_callback(const void* in, void* out, unsigned long frames,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags, void* user);
    int callback_impl(float* out, unsigned long frames);

    KopawGraph* g_;
    int rate_;
    int channels_;
    uint32_t node_id_ = 0;

    PaStream* stream_ = nullptr;
    kop::SpscRingF32 ring_;
    size_t prime_target_;      // 起播前预缓冲（浮点样本数）
    bool primed_ = false;
    std::atomic<bool> eos_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<uint64_t> consumed_frames_{0};
    std::atomic<int> underruns_{0};

    KopawOutput out_{};
};

}  // namespace kopaw
