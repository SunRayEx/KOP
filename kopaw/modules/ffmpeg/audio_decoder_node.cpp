#include "audio_decoder_node.hpp"

#include <cstring>

#include "../frame.hpp"
#include "kop/log.h"

namespace kopaw {

static const char* kTag = "adec";

AudioDecoderNode::~AudioDecoderNode() {
    swr_free(&swr_);
    if (pkt_) av_packet_free(&pkt_);
    if (frm_) av_frame_free(&frm_);
    if (ctx_) avcodec_free_context(&ctx_);
    av_channel_layout_uninit(&out_layout_);
}

namespace {
KopawNodeVTable make_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* f) -> int32_t {
        return static_cast<AudioDecoderNode*>(user)->send_impl(f);
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<AudioDecoderNode*>(user); };
    return vt;
}
const KopawNodeVTable kVTable = make_vtable();
}  // namespace

bool AudioDecoderNode::open(AVCodecParameters* params, std::string* error) {
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        *error = "找不到音频解码器";
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_, params) < 0) {
        *error = "音频解码参数拷贝失败";
        return false;
    }
    ctx_->pkt_timebase = AVRational{1, 1000000};
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        *error = "音频解码器打开失败";
        return false;
    }
    av_channel_layout_default(&out_layout_, out_channels_);
    pkt_ = av_packet_alloc();
    frm_ = av_frame_alloc();
    if (!pkt_ || !frm_) {
        *error = "AVPacket/AVFrame 分配失败";
        return false;
    }
    return true;
}

KopawNodeDesc AudioDecoderNode::desc(KopawGraph* g) {
    set_graph(g);
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "audio_decoder";
    d.user_data = this;
    d.outputs = 1;
    d.inputs = 1;
    d.queue_capacity = 64;
    d.is_sink = 0;
    d.self_driven = 0;
    d.vtable = &kVTable;
    return d;
}

bool AudioDecoderNode::ensure_resampler(const AVFrame* frame) {
    if (swr_) return true;
    int rc = swr_alloc_set_opts2(&swr_, &out_layout_, AV_SAMPLE_FMT_FLT, out_rate_,
                                 &frame->ch_layout,
                                 static_cast<AVSampleFormat>(frame->format),
                                 frame->sample_rate, 0, nullptr);
    if (rc < 0 || !swr_) {
        KOP_LOG_ERROR(kTag, "swr 上下文创建失败");
        return false;
    }
    if (swr_init(swr_) < 0) {
        KOP_LOG_ERROR(kTag, "swr_init 失败");
        return false;
    }
    return true;
}

int32_t AudioDecoderNode::emit_resampled(AVFrame* frame) {
    if (!ensure_resampler(frame)) return KOPAW_E_GENERIC;

    int max_out = swr_get_out_samples(swr_, frame->nb_samples);
    if (max_out <= 0) max_out = frame->nb_samples;
    const size_t bytes_per_frame = static_cast<size_t>(out_channels_) * sizeof(float);
    // P1 帧池化：PCM 输出块经全局池回收，稳态零 malloc
    OwnedFrame* o =
        FramePool::acquire(KOPAW_MEDIA_AUDIO, static_cast<size_t>(max_out) * bytes_per_frame);
    o->frame.pts = frame->pts;
    o->frame.dts = frame->pts;

    uint8_t* dst[1] = {o->data()};
    int n = swr_convert(swr_, dst, max_out, const_cast<const uint8_t**>(frame->data),
                        frame->nb_samples);
    if (n < 0) {
        o->frame.release(o->ptr());
        return KOPAW_E_GENERIC;
    }
    o->frame.size = static_cast<size_t>(n) * bytes_per_frame;
    o->frame.format.audio.sample_rate = static_cast<uint32_t>(out_rate_);
    o->frame.format.audio.channels = static_cast<uint32_t>(out_channels_);
    return kopaw_graph_emit(g_, out_, o->ptr());
}

void AudioDecoderNode::flush_and_finish() {
    avcodec_send_packet(ctx_, nullptr);
    while (avcodec_receive_frame(ctx_, frm_) >= 0) {
        if (emit_resampled(frm_) != KOPAW_OK) break;
        av_frame_unref(frm_);
    }
    // 冲刷重采样器残余
    if (swr_) {
        uint8_t* dst[1] = {nullptr};
        // 拉取缓冲中的残余样本
        int64_t delayed = swr_get_delay(swr_, out_rate_);
        if (delayed > 0) {
            const size_t bpf = static_cast<size_t>(out_channels_) * sizeof(float);
            OwnedFrame* o =
                make_frame(KOPAW_MEDIA_AUDIO, 0, 0, static_cast<size_t>(delayed) * bpf);
            uint8_t* dstbuf[1] = {o->data()};
            int n = swr_convert(swr_, dstbuf, delayed, nullptr, 0);
            if (n > 0) {
                o->frame.size = static_cast<size_t>(n) * bpf;
                o->frame.format.audio.sample_rate = static_cast<uint32_t>(out_rate_);
                o->frame.format.audio.channels = static_cast<uint32_t>(out_channels_);
                kopaw_graph_emit(g_, out_, o->ptr());
            } else {
                o->frame.release(o->ptr());
            }
        }
    }
    OwnedFrame* eos = make_frame(KOPAW_MEDIA_AUDIO, 0, 0, 0);
    eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
    kopaw_graph_emit(g_, out_, eos->ptr());
}

int32_t AudioDecoderNode::send_impl(KopawFrame* f) {
    if (f->flags & KOPAW_FRAME_FLAG_EOS) {
        // 契约：帧已接管并释放；无论冲刷/下游发射结果如何都返回 OK
        f->release(f);
        flush_and_finish();
        return KOPAW_OK;
    }

    // 包帧数据拷入 FFmpeg 自有缓冲（失败时帧未消费，返回非 OK 由引擎释放）
    const uint8_t* input = cpu_data(f);
    if (!input) return KOPAW_E_INVALID;
    if (av_new_packet(pkt_, static_cast<int>(f->size)) < 0) {
        return KOPAW_E_GENERIC;
    }
    memcpy(pkt_->data, input, f->size);
    pkt_->pts = f->pts;
    pkt_->dts = f->dts;
    pkt_->flags = (f->flags & KOPAW_FRAME_FLAG_KEY) ? AV_PKT_FLAG_KEY : 0;
    f->release(f);

    int rc = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        KOP_LOG_WARN(kTag, "send_packet 错误 %d，跳过该包", rc);
        return KOPAW_OK;
    }

    while (true) {
        rc = avcodec_receive_frame(ctx_, frm_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            KOP_LOG_WARN(kTag, "receive_frame 错误 %d", rc);
            break;
        }
        int32_t erc = emit_resampled(frm_);
        av_frame_unref(frm_);
        if (erc != KOPAW_OK) return KOPAW_OK;
    }
    return KOPAW_OK;
}

}  // namespace kopaw
