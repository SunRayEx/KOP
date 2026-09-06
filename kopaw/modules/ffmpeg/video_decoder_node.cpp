#include "video_decoder_node.hpp"
#include <string>
#include <cstdlib>

#include <cstring>

#include "../frame.hpp"
#include "kop/log.h"

namespace kopaw {

static const char* kTag = "vdec";

namespace {
KopawNodeVTable make_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* f) -> int32_t {
        return static_cast<VideoDecoderNode*>(user)->send_impl(f);
    };
    vt.stop = [](void*) {};  // send 是同步处理，无需额外停止点
    vt.destroy = [](void* user) { delete static_cast<VideoDecoderNode*>(user); };
    return vt;
}
const KopawNodeVTable kVTable = make_vtable();
}  // namespace

VideoDecoderNode::~VideoDecoderNode() {
    sws_freeContext(sws_);
    if (pkt_) av_packet_free(&pkt_);
    if (frm_) av_frame_free(&frm_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_.device_ref) av_buffer_unref(&hw_.device_ref);
}

enum AVPixelFormat VideoDecoderNode::get_hw_format(AVCodecContext* ctx,
                                                   const enum AVPixelFormat* fmts) {
    auto* self = static_cast<VideoDecoderNode*>(ctx->opaque);
    if (getenv("KOP_LOG") && getenv("KOP_LOG")[0] == 'd') {
        std::string lst;
        for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            lst += std::to_string(*p) + " ";
        }
        KOP_LOG_DEBUG(kTag, "get_format: hw=%d fmts=[%s]", self->hw_.hw_pix_fmt,
                      lst.c_str());
    }
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->hw_.hw_pix_fmt) return *p;
    }
    // 运行期优雅回退：解码器可能在 hw 初始化失败后以纯软格式列表重新协商
    // （老编码/驱动常见）。接受首个软格式，并动态关闭硬解帧路径。
    KOP_LOG_WARN(kTag, "硬解协商失败（%s），运行期回退软解", self->hw_.name.c_str());
    self->hw_active_ = false;
    return fmts[0];
}

bool VideoDecoderNode::open(AVCodecParameters* params, std::string* error) {
    // 硬解探测：成功则先试硬解，打开失败自动回退软解
    if (hw_probe(params->codec_id, &hw_)) {
        if (open_context(params, true, error)) {
            hw_active_ = true;
            KOP_LOG_INFO(kTag, "视频硬解已启用：%s", hw_.name.c_str());
            return true;
        }
        KOP_LOG_WARN(kTag, "硬解打开失败（%s），回退软解", error->c_str());
        close_context();
        if (hw_.device_ref) av_buffer_unref(&hw_.device_ref);
        hw_ = HwAccelConfig{};
    }
    if (!open_context(params, false, error)) return false;
    hw_active_ = false;
    KOP_LOG_INFO(kTag, "视频软解已启用");
    return true;
}

bool VideoDecoderNode::open_context(AVCodecParameters* params, bool hw, std::string* error) {
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        *error = "找不到视频解码器";
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_, params) < 0) {
        *error = "视频解码参数拷贝失败";
        return false;
    }
    // 包帧的 pts/dts 以微秒计（demuxer 已转换），据此告知解码器输出时间基
    ctx_->pkt_timebase = AVRational{1, 1000000};
    if (hw) {
        ctx_->opaque = this;
        ctx_->get_format = &VideoDecoderNode::get_hw_format;
        ctx_->hw_device_ctx = av_buffer_ref(hw_.device_ref);
    }
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        *error = hw ? "硬解 avcodec_open2 失败" : "视频解码器打开失败";
        return false;
    }
    pkt_ = av_packet_alloc();
    frm_ = av_frame_alloc();
    if (!pkt_ || !frm_) {
        *error = "AVPacket/AVFrame 分配失败";
        return false;
    }
    return true;
}

void VideoDecoderNode::close_context() {
    sws_freeContext(sws_);
    sws_ = nullptr;
    if (pkt_) av_packet_free(&pkt_);
    if (frm_) av_frame_free(&frm_);
    if (ctx_) avcodec_free_context(&ctx_);
}

KopawNodeDesc VideoDecoderNode::desc(KopawGraph* g) {
    set_graph(g);
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "video_decoder";
    d.user_data = this;
    d.outputs = 1;
    d.inputs = 1;
    d.queue_capacity = 32;  // 包帧轻量
    d.is_sink = 0;
    d.self_driven = 0;
    d.vtable = &kVTable;
    return d;
}

int32_t VideoDecoderNode::emit_converted(AVFrame* frame) {
    // 源参数变化（少见）时重建 sws 上下文
    if (!sws_ || sws_src_w_ != frame->width || sws_src_h_ != frame->height ||
        sws_src_fmt_ != frame->format) {
        sws_freeContext(sws_);
        sws_ = sws_getContext(frame->width, frame->height,
                              static_cast<AVPixelFormat>(frame->format), frame->width,
                              frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                              nullptr, nullptr);
        if (!sws_) return KOPAW_E_GENERIC;
        sws_src_w_ = frame->width;
        sws_src_h_ = frame->height;
        sws_src_fmt_ = frame->format;
    }

    const size_t dst_size = static_cast<size_t>(frame->width) * frame->height * 4;
    // P1 帧池化：RGBA 大块经全局池回收，稳态零 malloc
    OwnedFrame* o = FramePool::acquire(KOPAW_MEDIA_VIDEO, dst_size);
    o->frame.pts = frame->pts;
    o->frame.dts = frame->pts;
    o->frame.format.video.width = static_cast<uint32_t>(frame->width);
    o->frame.format.video.height = static_cast<uint32_t>(frame->height);
    o->frame.stride = static_cast<uint32_t>(frame->width) * 4;

    const uint8_t* src[4] = {frame->data[0], frame->data[1], frame->data[2],
                             frame->data[3]};
    const int src_stride[4] = {frame->linesize[0], frame->linesize[1],
                               frame->linesize[2], frame->linesize[3]};
    uint8_t* dst[1] = {o->data()};
    const int dst_stride[1] = {static_cast<int>(frame->width) * 4};
    sws_scale(sws_, src, src_stride, 0, frame->height, dst, dst_stride);

    return kopaw_graph_emit(g_, out_, o->ptr());
}

int32_t VideoDecoderNode::decode_frame_to_rgba(AVFrame* raw) {
    if (!hw_active_ || raw->format != hw_.hw_pix_fmt) {
        return emit_converted(raw);
    }
    // 硬解帧在加速器内存：拉回系统内存（NV12，转换交给 sws）
    AVFrame* sw = av_frame_alloc();
    if (!sw) return KOPAW_E_GENERIC;
    if (av_hwframe_transfer_data(sw, raw, 0) < 0 || sw->data[0] == nullptr) {
        KOP_LOG_WARN(kTag, "硬解帧回传失败，丢弃该帧");
        av_frame_free(&sw);
        return KOPAW_OK;
    }
    sw->pts = raw->pts;
    int32_t rc = emit_converted(sw);
    av_frame_free(&sw);
    return rc;
}

void VideoDecoderNode::flush_and_finish() {
    // 冲刷解码器残余帧
    avcodec_send_packet(ctx_, nullptr);
    while (avcodec_receive_frame(ctx_, frm_) >= 0) {
        decode_frame_to_rgba(frm_);
        av_frame_unref(frm_);
    }
    OwnedFrame* eos = make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0);
    eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
    kopaw_graph_emit(g_, out_, eos->ptr());
}

int32_t VideoDecoderNode::send_impl(KopawFrame* f) {
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
        int32_t erc = decode_frame_to_rgba(frm_);
        av_frame_unref(frm_);
        if (erc != KOPAW_OK) return KOPAW_OK;  // 下游停止：静默退出
    }
    return KOPAW_OK;
}

}  // namespace kopaw
