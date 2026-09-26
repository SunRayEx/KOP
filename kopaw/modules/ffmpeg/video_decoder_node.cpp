#include "video_decoder_node.hpp"
#include <string>
#include <cstdlib>

#include <cstring>

#include "ffmpeg_error.hpp"

#if KOPAW_HAVE_LIBVA
#include <libavutil/hwcontext_vaapi.h>  // AVVAAPIDeviceContext
#endif

#include "../frame.hpp"
#include "color_metadata.hpp"
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

VideoDecoderNode::~VideoDecoderNode() = default;

bool VideoDecoderNode::native_dmabuf_export_supported() const {
#if KOPAW_HAVE_LIBVA
    return hw_active_ && hw_.type == AV_HWDEVICE_TYPE_VAAPI;
#else
    return false;
#endif
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
        hw_.device_ref.reset();
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
    ctx_.reset(avcodec_alloc_context3(codec));
    if (!ctx_) return false;
    if (avcodec_parameters_to_context(ctx_.get(), params) < 0) {
        *error = "视频解码参数拷贝失败";
        return false;
    }
    // 包帧的 pts/dts 以微秒计（demuxer 已转换），据此告知解码器输出时间基
    ctx_->pkt_timebase = AVRational{1, 1000000};
    if (hw) {
        ctx_->opaque = this;
        ctx_->get_format = &VideoDecoderNode::get_hw_format;
        ctx_->hw_device_ctx = av_buffer_ref(hw_.device_ref.get());
    }
    if (avcodec_open2(ctx_.get(), codec, nullptr) < 0) {
        *error = hw ? "硬解 avcodec_open2 失败" : "视频解码器打开失败";
        return false;
    }
    if (!pkt_ || !frm_) {
        *error = "AVPacket/AVFrame 分配失败";
        return false;
    }
    return true;
}

void VideoDecoderNode::close_context() {
    sws_.reset();
    pkt_.reset();
    frm_.reset();
    ctx_.reset();
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
        sws_.reset(sws_getContext(frame->width, frame->height,
                                  static_cast<AVPixelFormat>(frame->format), frame->width,
                                  frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                  nullptr, nullptr));
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
    o->frame.color = color_metadata_from_av_frame(frame);

    const uint8_t* src[4] = {frame->data[0], frame->data[1], frame->data[2],
                             frame->data[3]};
    const int src_stride[4] = {frame->linesize[0], frame->linesize[1],
                               frame->linesize[2], frame->linesize[3]};
    uint8_t* dst[1] = {o->data()};
    const int dst_stride[1] = {static_cast<int>(frame->width) * 4};
    sws_scale(sws_.get(), src, src_stride, 0, frame->height, dst, dst_stride);

    return kopaw_graph_emit(g_, out_, o->ptr());
}

int32_t VideoDecoderNode::decode_frame_to_rgba(AVFrame* raw) {
    if (!hw_active_ || raw->format != hw_.hw_pix_fmt) {
        return emit_converted(raw);
    }
    // P2 硬解零拷贝：优先原生 DMA-BUF 导出（VAAPI），失败回退拉回路径
    if (OwnedFrame* native = try_export_native(raw)) {
        return kopaw_graph_emit(g_, out_, native->ptr());
    }
    // 硬解帧在加速器内存：拉回系统内存（NV12，转换交给 sws）
    AvFrame sw;
    if (av_hwframe_transfer_data(sw.get(), raw, 0) < 0 || sw->data[0] == nullptr) {
        KOP_LOG_WARN(kTag, "硬解帧回传失败，丢弃该帧");
        return KOPAW_OK;
    }
    av_frame_copy_props(sw.get(), raw);
    sw->pts = raw->pts;
    return emit_converted(sw.get());
}

OwnedFrame* VideoDecoderNode::try_export_native(AVFrame* raw) {
#if KOPAW_HAVE_LIBVA
    if (!native_output_.load(std::memory_order_acquire) || native_failed_) return nullptr;
    const uint32_t accepted_formats =
        native_format_mask_.load(std::memory_order_acquire);
    if (accepted_formats == kNativeDmabufFormatNone) return nullptr;
    const char* zc = getenv("KOPAW_ZERO_COPY");
    if (zc && zc[0] == '0') return nullptr;
    if (!va_export_tried_) {
        va_export_tried_ = true;
        if (hw_.type != AV_HWDEVICE_TYPE_VAAPI || !hw_.device_ref ||
            !hw_.device_ref->data) {
            native_failed_ = true;
            KOP_LOG_INFO(kTag, "非 VAAPI 硬解，保持系统内存输出");
            return nullptr;
        }
        auto* hw_dev = reinterpret_cast<AVHWDeviceContext*>(hw_.device_ref->data);
        auto* dev = static_cast<AVVAAPIDeviceContext*>(hw_dev->hwctx);
        std::string err;
        if (!va_export_.init(dev->display)) {
            native_failed_ = true;
            KOP_LOG_INFO(kTag, "导出器初始化失败，保持系统内存输出");
            return nullptr;
        }
        KOP_LOG_INFO(kTag, "零拷贝输出：解码表面原生 DMA-BUF 导出（NV12/P010）");
    }
    std::string err;
    OwnedFrame* o = va_export_.export_frame(raw, accepted_formats, &err);
    if (!o) {
        // 首次失败即永久回退：逐帧重试只会刷日志（驱动/格式能力不变）
        native_failed_ = true;
        KOP_LOG_WARN(kTag, "原生导出不可用（%s），回退系统内存路径", err.c_str());
    }
    return o;
#else
    (void)raw;
    return nullptr;
#endif
}

void VideoDecoderNode::flush_and_finish() {
    // 冲刷解码器残余帧
    avcodec_send_packet(ctx_.get(), nullptr);
    while (avcodec_receive_frame(ctx_.get(), frm_.get()) >= 0) {
        decode_frame_to_rgba(frm_.get());
        frm_.unref();
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
    if (!pkt_.new_packet(static_cast<int>(f->size))) {
        return KOPAW_E_GENERIC;
    }
    memcpy(pkt_->data, input, f->size);
    pkt_->pts = f->pts;
    pkt_->dts = f->dts;
    pkt_->flags = (f->flags & KOPAW_FRAME_FLAG_KEY) ? AV_PKT_FLAG_KEY : 0;
    f->release(f);

    int rc = avcodec_send_packet(ctx_.get(), pkt_.get());
    pkt_.unref();
    if (rc < 0 && !av_is_retry(rc)) {
        KOP_LOG_WARN(kTag, "send_packet 错误 %s，跳过该包", av_error_string(rc));
        return KOPAW_OK;
    }

    while (true) {
        rc = avcodec_receive_frame(ctx_.get(), frm_.get());
        if (av_is_retry(rc)) break;
        if (rc < 0) {
            KOP_LOG_WARN(kTag, "receive_frame 错误 %s", av_error_string(rc));
            break;
        }
        int32_t erc = decode_frame_to_rgba(frm_.get());
        frm_.unref();
        if (erc != KOPAW_OK) return KOPAW_OK;  // 下游停止：静默退出
    }
    return KOPAW_OK;
}

}  // namespace kopaw
