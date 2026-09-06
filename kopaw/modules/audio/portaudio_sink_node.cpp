#include "portaudio_sink_node.hpp"

#include <cstring>

#include "../frame.hpp"
#include "kop/log.h"
#include "kop/time.h"

namespace kopaw {

static const char* kTag = "asink";

namespace {
KopawNodeVTable make_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* f) -> int32_t {
        return static_cast<AudioSinkNode*>(user)->send_impl(f);
    };
    vt.stop = [](void* user) { static_cast<AudioSinkNode*>(user)->stop_impl(); };
    vt.destroy = [](void* user) { delete static_cast<AudioSinkNode*>(user); };
    return vt;
}
const KopawNodeVTable kVTable = make_vtable();
}  // namespace

AudioSinkNode::AudioSinkNode(KopawGraph* g, int rate, int channels)
    : g_(g),
      rate_(rate),
      channels_(channels),
      ring_(1u << 16),  // 65536 浮点样本 ≈ 0.68s（48k 立体声）
      prime_target_(static_cast<size_t>(rate) * 0.15 * channels) {}

AudioSinkNode::~AudioSinkNode() {
    if (stream_) {
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
}

bool AudioSinkNode::open(std::string* error) {
    PaError err = Pa_OpenDefaultStream(&stream_, 0, channels_, paFloat32, rate_,
                                       paFramesPerBufferUnspecified, pa_callback, this);
    if (err != paNoError) {
        *error = std::string("PortAudio 打开输出流失败: ") + Pa_GetErrorText(err);
        return false;
    }
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        *error = std::string("PortAudio 启动失败: ") + Pa_GetErrorText(err);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        return false;
    }
    return true;
}

KopawNodeDesc AudioSinkNode::desc() {
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "audio_sink";
    d.user_data = this;
    d.outputs = 0;
    d.inputs = 1;
    d.queue_capacity = 64;
    d.is_sink = 1;
    d.self_driven = 0;
    d.vtable = &kVTable;
    return d;
}

int32_t AudioSinkNode::send_impl(KopawFrame* f) {
    if (f->flags & KOPAW_FRAME_FLAG_EOS) {
        eos_.store(true, std::memory_order_release);
        f->release(f);
        return KOPAW_OK;
    }
    const uint8_t* input = cpu_data(f);
    if (!input) return KOPAW_E_INVALID;
    const float* p = reinterpret_cast<const float*>(input);
    size_t left = f->size / sizeof(float);
    while (left > 0) {
        size_t n = ring_.write(p, left);
        p += n;
        left -= n;
        if (left == 0) break;
        ring_.sync_consumer_pos();  // 刷新消费游标后再判满
        if (stopped_.load(std::memory_order_relaxed)) {
            // 契约：帧已由本节点接管并释放，必须返回 OK（引擎不得再释放）
            f->release(f);
            return KOPAW_OK;
        }
        kop::sleep_us(1000);
    }
    f->release(f);
    return KOPAW_OK;
}

void AudioSinkNode::stop_impl() {
    stopped_.store(true, std::memory_order_relaxed);
    if (stream_) Pa_AbortStream(stream_);
}

int AudioSinkNode::pa_callback(const void*, void* out, unsigned long frames,
                               const PaStreamCallbackTimeInfo*,
                               PaStreamCallbackFlags, void* user) {
    return static_cast<AudioSinkNode*>(user)->callback_impl(
        static_cast<float*>(out), frames);
}

int AudioSinkNode::callback_impl(float* out, unsigned long frames) {
    const size_t need = static_cast<size_t>(frames) * channels_;

    if (!primed_) {
        ring_.sync_producer_pos();
        if (ring_.read_space() < prime_target_) {
            memset(out, 0, need * sizeof(float));
            return stopped_.load(std::memory_order_relaxed) ? paComplete : paContinue;
        }
        primed_ = true;
    }

    size_t got = ring_.read(out, need);
    if (got < need) {
        memset(out + got, 0, (need - got) * sizeof(float));
        // EOS 排空期的零填充是正常收尾，不算欠载
        if (!eos_.load(std::memory_order_acquire) &&
            underruns_.fetch_add(1) == 0) {
            KOP_LOG_WARN(kTag, "音频欠载");
        }
    }
    ring_.sync_producer_pos();

    const uint64_t consumed = consumed_frames_.fetch_add(got / channels_) + got / channels_;
    // 主时钟：按已消费帧数上报媒体位置
    kopaw_graph_clock_set(g_, static_cast<int64_t>(consumed * 1000000ULL / rate_));

    // EOS 之后缓冲排空即完成播放（got < need 的最后一次也视为结束）。
    // P1 尾部修复：此刻才向引擎记账 sink_done——FINISHED 触发时
    // 全部缓冲音频都已真正播出，不再截断尾部。
    if (eos_.load(std::memory_order_acquire) && ring_.read_space() == 0) {
        KOP_LOG_DEBUG(kTag, "ring 排空，记账 sink_done（node=%u）", node_id_);
        if (node_id_ != 0) kopaw_node_sink_done(g_, node_id_);
        return paComplete;
    }
    return stopped_.load(std::memory_order_relaxed) ? paComplete : paContinue;
}

}  // namespace kopaw
