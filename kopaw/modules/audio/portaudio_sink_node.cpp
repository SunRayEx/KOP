#include "portaudio_sink_node.hpp"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "../frame.hpp"
#include "kop/log.h"
#include "kop/time.h"

namespace kopaw {

static const char* kTag = "asink";

namespace {

bool runtime_socket_exists(const char* runtime_dir, const char* name) {
    if (!runtime_dir || !runtime_dir[0]) return false;
    std::string path = std::string(runtime_dir) + "/" + name;
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

bool pipewire_audio_detected() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0]) return false;
    return runtime_socket_exists(runtime, "pipewire-0") ||
           runtime_socket_exists(runtime, "pulse/native");
}

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
    const char* backend = std::getenv("KOPAW_AUDIO_BACKEND");
    if (backend && std::strcmp(backend, "null") == 0) {
        null_backend_ = true;
        KOP_LOG_INFO(kTag, "音频输出后端: null（仅消费数据，不连接音频设备）");
        return true;
    }
    const char* requested = std::getenv("KOPAW_AUDIO_DEVICE");
    PaDeviceIndex device = paNoDevice;
    if (requested && requested[0] != '\0') {
        const int count = Pa_GetDeviceCount();
        for (int i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (info && std::strstr(info->name, requested)) {
                device = i;
                break;
            }
        }
        if (device == paNoDevice) {
            *error = std::string("找不到 PortAudio 输出设备: ") + requested;
            return false;
        }
    } else {
        // Do not force a PipeWire PCM or reorder PortAudio devices. Detect the
        // desktop audio server, then take ownership of the system default
        // output selected by WirePlumber/Pulse compatibility. This preserves
        // the user's routing and also works on plain ALSA systems.
        const bool pipewire = pipewire_audio_detected();
        device = Pa_GetDefaultOutputDevice();
        if (pipewire) {
            KOP_LOG_INFO(kTag,
                         "检测到 PipeWire/Pulse 音频服务，接管系统默认输出（不覆盖路由）");
        } else {
            KOP_LOG_INFO(kTag, "未检测到 PipeWire/Pulse，使用 PortAudio 默认输出");
        }
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (!info) {
        *error = "PortAudio 没有可用的默认输出设备";
        return false;
    }
    const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
    KOP_LOG_INFO(kTag, "音频输出设备: %s（host=%s）", info->name,
                 api ? api->name : "unknown");
    PaStreamParameters output{};
    output.device = device;
    output.channelCount = channels_;
    output.sampleFormat = paFloat32;
    output.suggestedLatency = info->defaultLowOutputLatency;
    PaError err = Pa_OpenStream(&stream_, nullptr, &output, rate_,
                                paFramesPerBufferUnspecified, paNoFlag,
                                pa_callback, this);
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
        if (null_backend_ && node_id_ != 0) kopaw_node_sink_done(g_, node_id_);
        return KOPAW_OK;
    }
    const uint8_t* input = cpu_data(f);
    if (!input) return KOPAW_E_INVALID;
    const float* p = reinterpret_cast<const float*>(input);
    size_t left = f->size / sizeof(float);
    if (null_backend_) {
        const size_t frames = left / static_cast<size_t>(channels_);
        const uint64_t consumed = consumed_frames_.fetch_add(frames) + frames;
        kopaw_graph_clock_set(g_, static_cast<int64_t>(consumed * 1000000ULL / rate_));
        f->release(f);
        return KOPAW_OK;
    }
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
