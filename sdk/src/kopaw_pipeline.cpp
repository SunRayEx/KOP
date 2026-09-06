#include "kop/sdk/kopaw_pipeline.hpp"

#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

#include "frame.hpp"

#include <chrono>

#include "kop/log.h"
#include "kopaw_abi.h"

// 内部节点 API（与 kopaw-player 同一装配层）
#include "audio/portaudio_sink_node.hpp"
#include "ffmpeg/audio_decoder_node.hpp"
#include "ffmpeg/demuxer_node.hpp"
#include "ffmpeg/filter_node.hpp"
#include "ffmpeg/video_decoder_node.hpp"
#include "portaudio.h"

namespace kop {
namespace sdk {

static const char* kTag = "kop-sdk";

namespace {
int64_t now_ms_impl() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// ---------------------------------------------------------------------------
// 回调汇聚节点：把解码输出转成 FrameView 交给用户回调（无窗口、纯数据面）。
// 所有权与内部节点一致：引擎移交帧 → 回调 → 释放；EOS 时显式 sink_done 记账。
// ---------------------------------------------------------------------------
class FrameSinkNode {
public:
    std::function<void(const FrameView&)> callback;
    KopawGraph* g = nullptr;
    uint32_t node_id = 0;

    KopawNodeDesc desc() {
        KopawNodeDesc d{};
        d.struct_size = sizeof(d);
        d.name = "sdk_frame_sink";
        d.user_data = this;
        d.outputs = 0;
        d.inputs = 1;
        d.queue_capacity = 4;
        d.is_sink = 1;
        d.self_driven = 0;
        static KopawNodeVTable vt = [] {
            KopawNodeVTable v{};
            v.struct_size = sizeof(v);
            v.send = [](void* user, KopawFrame* frame) -> int32_t {
                static_cast<FrameSinkNode*>(user)->send_impl(frame);
                return KOPAW_OK;
            };
            v.stop = [](void*) {};
            v.destroy = [](void* user) { delete static_cast<FrameSinkNode*>(user); };
            return v;
        }();
        d.vtable = &vt;
        return d;
    }

    void send_impl(KopawFrame* f) {
        if (f->flags & KOPAW_FRAME_FLAG_EOS) {
            f->release(f);
            if (g) kopaw_node_sink_done(g, node_id);
            return;
        }
        if (callback) {
            FrameView view{};
            view.media_type = f->media_type;
            view.eos = false;
            view.pts = f->pts;
            view.dts = f->dts;
            if (f->media_type == KOPAW_MEDIA_VIDEO) {
                view.width = f->format.video.width;
                view.height = f->format.video.height;
                view.stride = f->stride;
                view.data = reinterpret_cast<const uint8_t*>(f->dma_buf_handle);
                view.size = f->size;
            } else {
                view.data = reinterpret_cast<const uint8_t*>(f->dma_buf_handle);
                view.size = f->size;
            }
            callback(view);
        }
        f->release(f);
    }
};

// ---------------------------------------------------------------------------
struct Pipeline::Impl {
    PipelineOptions options;

    // 阶段一对象（open 失败时手动清理；入图后由 graph_free 的 destroy 回收）
    kopaw::DemuxerNode* demux = nullptr;
    kopaw::VideoDecoderNode* vdec = nullptr;
    kopaw::VideoFilterNode* vfilter = nullptr;
    kopaw::AudioDecoderNode* adec = nullptr;
    kopaw::AudioFilterNode* afilter = nullptr;
    kopaw::AudioSinkNode* asink = nullptr;
    FrameSinkNode* frame_sink = nullptr;
    bool pa_inited = false;
    // 逐节点入图标记：graph_free 只回收已入图节点；未入图的由这里精确回收，
    // 避免双重释放（open 中途失败与正常析构共用一条清理路径）。
    bool added_demux = false;
    bool added_vdec = false;
    bool added_vfilter = false;
    bool added_adec = false;
    bool added_afilter = false;
    bool added_asink = false;
    bool added_sink = false;

    KopawGraph* g = nullptr;
    uint64_t frames_received = 0;

    ~Impl() { cleanup_pre_graph(); }

    void cleanup_pre_graph() {
        // 已入图节点由 kopaw_graph_free 的 destroy 回调回收；
        // 这里只删未入图的（open 中途失败遗留）。
        if (!added_asink) delete asink;
        if (!added_afilter) delete afilter;
        if (!added_adec) delete adec;
        if (!added_sink) delete frame_sink;
        if (!added_vfilter) delete vfilter;
        if (!added_vdec) delete vdec;
        if (!added_demux) delete demux;
        asink = nullptr;
        afilter = nullptr;
        adec = nullptr;
        frame_sink = nullptr;
        vfilter = nullptr;
        vdec = nullptr;
        demux = nullptr;
        if (pa_inited) {
            Pa_Terminate();
            pa_inited = false;
        }
    }
};

Pipeline::~Pipeline() {
    if (impl_) {
        if (impl_->g) {
            kopaw_graph_stop(impl_->g, 2000);
            kopaw_graph_free(impl_->g);
            impl_->g = nullptr;
        }
        impl_->cleanup_pre_graph();
    }
}

std::unique_ptr<Pipeline> Pipeline::open(const PipelineOptions& options,
                                         std::string* error) {
    auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
    auto* impl = new Impl();
    pipeline->impl_ = std::unique_ptr<Impl>(impl);
    impl->options = options;
    // 失败路径直接 return nullptr：unique_ptr 析构 → ~Pipeline → 图/节点清理
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::unique_ptr<Pipeline>();
    };

    if (options.media.empty()) return fail("媒体路径为空");
    if (!options.on_frame) return fail("缺少 on_frame 回调");

    if (options.audio) {
        if (Pa_Initialize() != paNoError) return fail("PortAudio 初始化失败");
        impl->pa_inited = true;
    }

    auto* demux = new kopaw::DemuxerNode();
    impl->demux = demux;
    demux->set_io_policy(options.timeout_ms, options.buffer_ms);
    std::vector<std::pair<std::string, std::string>> input_opts;
    if (!demux->open(options.media, input_opts, error)) {
        return fail("打开媒体失败: " + (error ? *error : ""));
    }
    const bool use_video = options.video && demux->has_video();
    const bool use_audio = options.audio && demux->has_audio();
    if (!use_video && !use_audio) return fail("媒体中没有可用的流");

    if (use_video) {
        impl->vdec = new kopaw::VideoDecoderNode();
        if (!impl->vdec->open(demux->video_codec_params(), error)) {
            return fail("视频解码器打开失败: " + (error ? *error : ""));
        }
        if (!options.video_filter.empty()) {
            impl->vfilter = new kopaw::VideoFilterNode();
            if (!impl->vfilter->open(options.video_filter, error)) {
                return fail("视频滤镜打开失败: " + (error ? *error : ""));
            }
        }
        impl->frame_sink = new FrameSinkNode();
        impl->frame_sink->callback = options.on_frame;
    }
    if (use_audio) {
        impl->adec = new kopaw::AudioDecoderNode();
        if (!impl->adec->open(demux->audio_codec_params(), error)) {
            return fail("音频解码器打开失败: " + (error ? *error : ""));
        }
        if (!options.audio_filter.empty()) {
            impl->afilter = new kopaw::AudioFilterNode();
            if (!impl->afilter->open(options.audio_filter, error)) {
                return fail("音频滤镜打开失败: " + (error ? *error : ""));
            }
        }
        impl->asink = new kopaw::AudioSinkNode(nullptr);
        if (!impl->asink->open(error)) {
            return fail("音频设备打开失败: " + (error ? *error : ""));
        }
    }

    // ---- 入图（此后节点对象由 graph_free 的 destroy 回调负责）----
    impl->g = kopaw_graph_new();
    if (!impl->g) return fail("图创建失败");
    kopaw::DemuxerNode* demux_ptr = impl->demux;

    KopawNodeDesc d_demux = demux_ptr->desc(impl->g);
    const uint32_t demux_id = kopaw_graph_add_node(impl->g, &d_demux);
    if (demux_id == 0) return fail("解复用节点注册失败");
    impl->added_demux = true;

    uint32_t vdec_id = 0;
    uint32_t vfilter_id = 0;
    uint32_t sink_id = 0;
    uint32_t adec_id = 0;
    uint32_t afilter_id = 0;
    uint32_t asink_id = 0;
    bool wired = true;

    if (impl->vdec) {
        KopawNodeDesc d = impl->vdec->desc(impl->g);
        vdec_id = kopaw_graph_add_node(impl->g, &d);
        impl->added_vdec = vdec_id != 0;
        impl->vdec->set_output(kopaw_graph_node_output(impl->g, vdec_id, 0));
        if (vdec_id == 0) wired = false;
        if (impl->vfilter) {
            KopawNodeDesc df = impl->vfilter->desc(impl->g);
            vfilter_id = kopaw_graph_add_node(impl->g, &df);
            impl->added_vfilter = vfilter_id != 0;
            impl->vfilter->set_output(kopaw_graph_node_output(impl->g, vfilter_id, 0));
            if (vfilter_id == 0 ||
                kopaw_graph_connect(impl->g,
                                    kopaw_graph_node_output(impl->g, vdec_id, 0),
                                    vfilter_id, 0, 0) != KOPAW_OK) {
                wired = false;
            }
        }
        if (impl->frame_sink) {
            KopawNodeDesc ds = impl->frame_sink->desc();
            sink_id = kopaw_graph_add_node(impl->g, &ds);
            impl->added_sink = sink_id != 0;
            impl->frame_sink->g = impl->g;
            impl->frame_sink->node_id = sink_id;
            const uint32_t tail = vfilter_id ? vfilter_id : vdec_id;
            if (sink_id == 0 ||
                kopaw_graph_connect(impl->g, kopaw_graph_node_output(impl->g, tail, 0),
                                    sink_id, 0, 0) != KOPAW_OK) {
                wired = false;
            }
        }
    }
    if (impl->adec) {
        KopawNodeDesc da = impl->adec->desc(impl->g);
        adec_id = kopaw_graph_add_node(impl->g, &da);
        impl->added_adec = adec_id != 0;
        impl->adec->set_output(kopaw_graph_node_output(impl->g, adec_id, 0));
        if (impl->afilter) {
            KopawNodeDesc df = impl->afilter->desc(impl->g);
            afilter_id = kopaw_graph_add_node(impl->g, &df);
            impl->added_afilter = afilter_id != 0;
            impl->afilter->set_output(kopaw_graph_node_output(impl->g, afilter_id, 0));
        }
        KopawNodeDesc dk = impl->asink->desc();
        asink_id = kopaw_graph_add_node(impl->g, &dk);
        impl->added_asink = asink_id != 0;
        impl->asink->set_graph(impl->g);
        impl->asink->set_node_id(asink_id);
        const uint32_t audio_tail = afilter_id ? afilter_id : adec_id;
        if (adec_id == 0 || asink_id == 0 ||
            kopaw_graph_connect(impl->g, kopaw_graph_node_output(impl->g, demux_id, 1),
                                adec_id, 0, 0) != KOPAW_OK ||
            (afilter_id != 0 &&
             kopaw_graph_connect(impl->g,
                                 kopaw_graph_node_output(impl->g, adec_id, 0),
                                 afilter_id, 0, 0) != KOPAW_OK) ||
            kopaw_graph_connect(impl->g,
                                kopaw_graph_node_output(impl->g, audio_tail, 0),
                                asink_id, 0, 0) != KOPAW_OK) {
            wired = false;
        }
    }
    demux_ptr->set_outputs(kopaw_graph_node_output(impl->g, demux_id, 0),
                           kopaw_graph_node_output(impl->g, demux_id, 1));
    // 视频源连线：demux.0 → vdec（set_output 已绑句柄，这里接边）
    if (impl->vdec &&
        kopaw_graph_connect(impl->g, kopaw_graph_node_output(impl->g, demux_id, 0),
                            vdec_id, 0, 0) != KOPAW_OK) {
        wired = false;
    }
    if (!wired) return fail("管线连线失败");

    if (options.on_log) options.on_log("pipeline opened: " + options.media);
    return pipeline;
}

bool Pipeline::start(std::string* error) {
    if (!impl_ || !impl_->g) {
        if (error) *error = "管线未打开";
        return false;
    }
    if (kopaw_graph_start(impl_->g) != KOPAW_OK) {
        if (error) *error = "图启动失败";
        return false;
    }
    return true;
}

int Pipeline::stop(uint32_t timeout_ms) {
    if (!impl_ || !impl_->g) return KOPAW_STATE_IDLE;
    kopaw_graph_stop(impl_->g, timeout_ms);
    return kopaw_graph_state(impl_->g);
}

int Pipeline::wait_finished(uint32_t timeout_ms) {
    if (!impl_ || !impl_->g) return KOPAW_STATE_IDLE;
    const int64_t deadline = now_ms_impl() + static_cast<int64_t>(timeout_ms);
    for (;;) {
        const int state = kopaw_graph_state(impl_->g);
        if (state == KOPAW_STATE_FINISHED || state == KOPAW_STATE_ERROR) {
            return state;
        }
        if (now_ms_impl() > deadline) return state;
        struct timespec ts{0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
}

int Pipeline::state() const {
    return impl_ && impl_->g ? kopaw_graph_state(impl_->g) : KOPAW_STATE_IDLE;
}

std::string Pipeline::stats_json() const {
    if (!impl_ || !impl_->g) return "{}";
    std::string buf(8192, '\0');
    const int n = kopaw_graph_stats_json(impl_->g, buf.data(),
                                         static_cast<uint32_t>(buf.size()));
    if (n <= 0) return "{}";
    buf.resize(static_cast<size_t>(n));
    return buf;
}

}  // namespace sdk
}  // namespace kop
