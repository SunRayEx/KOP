// 视频解码节点（响应式）：收包帧 → 解码（硬解探测/回退软解）→
// swscale → 池化 RGBA 帧发射。
// P2 硬解零拷贝：set_native_output(true) 且 VAAPI 硬解激活时，解码表面经
// vaExportSurfaceHandle 原生导出为 KOPAW_MEMORY_DMABUF（NV12/P010）帧直发下游；
// 导出不可用时自动回退系统内存路径。CPU 帧路径行为不变。
#pragma once
#include <atomic>
#include <string>

// FFmpeg 8 起公共头不再自带 extern "C" 保护，C++ 使用方必须自行包裹。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include "kopaw_abi.h"
#include "hwaccel.hpp"
#include "native_dmabuf.hpp"
#if KOPAW_HAVE_LIBVA
#include "vaapi_export.hpp"
#endif

namespace kopaw {

class VideoDecoderNode {
public:
    VideoDecoderNode() = default;
    ~VideoDecoderNode();

    // params 来自 DemuxerNode（仅构造期使用）；打开解码器失败返回 false。
    bool open(AVCodecParameters* params, std::string* error);

    KopawNodeDesc desc(KopawGraph* g);
    void set_output(KopawOutput out) { out_ = out; }
    void set_graph(KopawGraph* g) { g_ = g; }
    // 请求原生 DMA-BUF 输出（须在图 start 前调用）。仅当 VAAPI 硬解激活且
    // 驱动支持导出时生效；KOPAW_ZERO_COPY=0 可强制关闭。
    void set_native_output(bool on) { native_output_.store(on, std::memory_order_release); }
    bool native_output() const {
        return native_output_.load(std::memory_order_acquire);
    }
    void set_native_dmabuf_format_mask(uint32_t formats) {
        native_format_mask_.store(formats, std::memory_order_release);
    }
    uint32_t native_dmabuf_format_mask() const {
        return native_format_mask_.load(std::memory_order_acquire);
    }
    // CUDA/CUVID remains CPU-fallback only until a CUDA-EGL/DMA-BUF interop
    // exporter exists; do not advertise a synthetic native path.
    bool native_dmabuf_export_supported() const;

    int32_t send_impl(KopawFrame* f);

private:
    static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                            const enum AVPixelFormat* fmts);

    bool open_context(AVCodecParameters* params, bool hw, std::string* error);
    void close_context();
    int32_t emit_converted(AVFrame* frame);       // 返回 emit 结果码
    int32_t decode_frame_to_rgba(AVFrame* raw);   // 硬解帧拉回系统内存再转换
    OwnedFrame* try_export_native(AVFrame* raw);  // P2：原生导出（可空）
    void flush_and_finish();

    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    int sws_src_w_ = 0, sws_src_h_ = 0;
    int sws_src_fmt_ = -1;
    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;

    // 硬解状态
    HwAccelConfig hw_;
    bool hw_active_ = false;

    // P2 原生导出状态：惰性初始化；一次不可用后永久回退，避免每帧重试
    std::atomic<bool> native_output_{false};
    std::atomic<uint32_t> native_format_mask_{kNativeDmabufFormatNone};
    bool native_failed_ = false;
#if KOPAW_HAVE_LIBVA
    VaapiDmabufExporter va_export_;
    bool va_export_tried_ = false;
#endif

    KopawGraph* g_ = nullptr;
    KopawOutput out_{};
};

}  // namespace kopaw
