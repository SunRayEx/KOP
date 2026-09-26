#include "demuxer_node.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "../frame.hpp"
#include "kop/log.h"
#include "kopaw_abi.h"

namespace kopaw {

static const char* kTag = "demuxer";

// 微秒时间基
static const AVRational kUsTb = {1, 1000000};

DemuxerNode::~DemuxerNode() {
    io_.end();
    fmt_.reset();  // avformat_close_input
}

bool DemuxerNode::open(const std::string& path,
                       const std::vector<std::pair<std::string, std::string>>& opts,
                       std::string* error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "输入 URL 为空";
        return false;
    }
    stopped_.store(false, std::memory_order_release);
    video_stream_ = -1;
    audio_stream_ = -1;
    fmt_.reset();  // 重新打开前先归还旧上下文

    // 网络协议初始化（幂等；http/rtsp 等输入必需）
    static const bool net = [] {
        avformat_network_init();
        return true;
    }();
    (void)net;

    const bool network = is_network_url(path);
    // 选项字典：avformat_open_input 成功时会接管并释放它，因此只在“到达
    // open 之前”的早返回路径手工释放（仅此一处）。
    AVDictionary* dict = nullptr;
    for (const auto& kv : opts) av_dict_set(&dict, kv.first.c_str(), kv.second.c_str(), 0);
    // rw_timeout 使用微秒；保留用户显式的协议选项优先级。
    if (network && !has_option(opts, "rw_timeout") && timeout_ms_ > 0) {
        av_dict_set_int(&dict, "rw_timeout", static_cast<int64_t>(timeout_ms_) * 1000, 0);
    }
    if (network && buffer_ms_ > 0 && !has_option(opts, "max_delay")) {
        av_dict_set_int(&dict, "max_delay", static_cast<int64_t>(buffer_ms_) * 1000, 0);
    }

    fmt_.reset(avformat_alloc_context());
    if (!fmt_) {
        av_dict_free(&dict);
        if (error) *error = "分配输入上下文失败";
        return false;
    }
    fmt_->interrupt_callback.callback = &ffmpeg_interrupt_callback;
    fmt_->interrupt_callback.opaque = &io_;
    io_.begin(network ? timeout_ms_ : 0);
    // avformat_open_input 接管传入的上下文（失败时释放并置空），因此先把
    // RAII 持有的裸指针交出，调用后再放回；dict 同时被该调用释放。
    AVFormatContext* raw = fmt_.release();
    int rc = avformat_open_input(&raw, path.c_str(), nullptr, &dict);
    io_.end();
    fmt_.reset(raw);  // 失败时 raw 已被置空
    if (rc < 0) {
        char buf[256];
        av_strerror(rc, buf, sizeof(buf));
        if (error) {
            *error = io_.was_timed_out() ? "打开网络输入超时: " : "打开输入失败: ";
            *error += buf;
        }
        fmt_.reset();  // 失败时 open 已释放并置空，此处幂等
        return false;
    }
    io_.begin(network ? timeout_ms_ : 0);
    rc = avformat_find_stream_info(fmt_.get(), nullptr);
    io_.end();
    if (rc < 0) {
        char buf[256];
        av_strerror(rc, buf, sizeof(buf));
        if (error) {
            *error = io_.was_timed_out() ? "探测网络流超时: " : "探测流信息失败: ";
            *error += buf;
        }
        fmt_.reset();
        return false;
    }
    video_stream_ = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_ = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (video_stream_ < 0 && audio_stream_ < 0) {
        if (error) *error = "输入中没有可播放的音视频流";
        fmt_.reset();
        return false;
    }
    KOP_LOG_INFO(kTag, "已打开 %s（视频流:%d 音频流:%d）", path.c_str(), video_stream_,
                 audio_stream_);
    return true;
}

namespace {
KopawNodeVTable make_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.run = [](void* user) -> int32_t {
        return static_cast<DemuxerNode*>(user)->run_impl();
    };
    vt.stop = [](void* user) { static_cast<DemuxerNode*>(user)->request_stop(); };
    vt.destroy = [](void* user) { delete static_cast<DemuxerNode*>(user); };
    return vt;
}
const KopawNodeVTable kVTable = make_vtable();
}  // namespace

KopawNodeDesc DemuxerNode::desc(KopawGraph* g) {
    g_ = g;
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "demuxer";
    d.user_data = this;
    d.outputs = 2;
    d.inputs = 0;
    // 包很小，容量给足以免 demux 阻塞拖慢另一路
    d.queue_capacity = 128;
    d.is_sink = 0;
    d.self_driven = 0;
    d.vtable = &kVTable;
    return d;
}

void DemuxerNode::emit_packet(AVPacket* pkt, AVRational tb, KopawOutput out,
                              int32_t media_type) {
    int64_t pts = pkt->pts == AV_NOPTS_VALUE
                      ? pkt->dts
                      : pkt->pts;
    int64_t dts = pkt->dts == AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (pts == AV_NOPTS_VALUE) pts = 0;
    if (dts == AV_NOPTS_VALUE) dts = pts;
    pts = av_rescale_q(pts, tb, kUsTb);
    dts = av_rescale_q(dts, tb, kUsTb);

    OwnedFrame* o = make_frame(media_type, pts, dts, static_cast<size_t>(pkt->size));
    if (pkt->size > 0) memcpy(o->data(), pkt->data, pkt->size);
    if (pkt->flags & AV_PKT_FLAG_KEY) o->frame.flags |= KOPAW_FRAME_FLAG_KEY;
    kopaw_graph_emit(g_, out, o->ptr());  // 停止时引擎接管/释放帧
}

void DemuxerNode::emit_eos(KopawOutput out, int32_t media_type) {
    OwnedFrame* o = make_frame(media_type, 0, 0, 0);
    o->frame.flags |= KOPAW_FRAME_FLAG_EOS;
    kopaw_graph_emit(g_, out, o->ptr());
}

int32_t DemuxerNode::run_impl() {
    AvPacket pkt;
    if (!pkt) return KOPAW_E_GENERIC;

    const bool network = fmt_ && fmt_->iformat && fmt_->url && is_network_url(fmt_->url);
    bool clean_eof = false;
    int32_t failure = KOPAW_OK;
    int64_t eagain_since = 0;
    const int64_t timeout_us = static_cast<int64_t>(timeout_ms_) * 1000;
    while (!stopped_.load(std::memory_order_acquire)) {
        const int64_t read_deadline =
            network && timeout_us > 0
                ? (eagain_since == 0 ? FfmpegIoControl::now_us() + timeout_us
                                     : eagain_since + timeout_us)
                : 0;
        io_.begin_until(read_deadline);
        int rc = av_read_frame(fmt_.get(), pkt.get());
        io_.end();
        if (rc < 0) {
            if (stopped_.load(std::memory_order_acquire)) {
                break;
            }
            if (rc == AVERROR_EOF) {
                clean_eof = true;
                break;
            }
            if (rc == AVERROR(EAGAIN)) {
                if (!network) {
                    KOP_LOG_ERROR(kTag, "本地输入返回暂时无数据: %s", av_err2str(rc));
                    failure = KOPAW_E_GENERIC;
                    break;
                }
                if (eagain_since == 0) eagain_since = FfmpegIoControl::now_us();
                if (timeout_us > 0 &&
                    FfmpegIoControl::now_us() - eagain_since >= timeout_us) {
                    KOP_LOG_ERROR(kTag, "网络输入在 %ums 内没有新数据", timeout_ms_);
                    failure = KOPAW_E_TIMEOUT;
                    break;
                }
                KOP_LOG_DEBUG(kTag, "网络输入暂时无数据，等待重试");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            char buf[256];
            av_strerror(rc, buf, sizeof(buf));
            if (io_.was_timed_out()) {
                KOP_LOG_ERROR(kTag, "网络输入超时（缓冲 %ums）: %s", buffer_ms_, buf);
                failure = KOPAW_E_TIMEOUT;
            } else {
                KOP_LOG_ERROR(kTag, "输入断连或读取失败: %s", buf);
                failure = KOPAW_E_GENERIC;
            }
            break;
        }
        eagain_since = 0;
        if (pkt->stream_index == video_stream_ && video_stream_ >= 0) {
            emit_packet(pkt.get(), fmt_->streams[video_stream_]->time_base, video_out_,
                        KOPAW_MEDIA_VIDEO);
        } else if (pkt->stream_index == audio_stream_ && audio_stream_ >= 0) {
            emit_packet(pkt.get(), fmt_->streams[audio_stream_]->time_base, audio_out_,
                        KOPAW_MEDIA_AUDIO);
        }
        pkt.unref();
    }
    if (clean_eof) {
        if (video_stream_ >= 0) emit_eos(video_out_, KOPAW_MEDIA_VIDEO);
        if (audio_stream_ >= 0) emit_eos(audio_out_, KOPAW_MEDIA_AUDIO);
    }
    return failure;
}

bool DemuxerNode::is_network_url(const std::string& path) {
    const auto colon = path.find(':');
    if (colon == std::string::npos) return false;
    const std::string scheme = path.substr(0, colon);
    return scheme == "rtsp" || scheme == "rtsps" || scheme == "http" ||
           scheme == "https" || scheme == "tcp" || scheme == "udp";
}

bool DemuxerNode::has_option(const std::vector<std::pair<std::string, std::string>>& opts,
                             const char* key) {
    for (const auto& kv : opts) {
        if (kv.first == key) return true;
    }
    return false;
}

}  // namespace kopaw
