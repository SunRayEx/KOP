#include "filter_node.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/error.h>
}

#include "../frame.hpp"
#include "kop/log.h"

namespace kopaw {

static const char* kTag = "lavfi";

namespace {

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

std::string ffmpeg_error(int rc) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(rc, text, sizeof(text)) < 0) return std::to_string(rc);
    return text;
}

bool frame_header_valid(const KopawFrame* frame) {
    // The filter node reads every field, including the release callback. A
    // shorter forward-compatible prefix cannot describe a usable input here.
    return frame && frame->struct_size >= sizeof(KopawFrame) && frame->release;
}

bool valid_video_frame(const KopawFrame* frame) {
    if (!frame_header_valid(frame) || frame->media_type != KOPAW_MEDIA_VIDEO ||
        frame->memory_type != KOPAW_MEMORY_CPU) {
        return false;
    }
    const uint32_t width = frame->format.video.width;
    const uint32_t height = frame->format.video.height;
    if (width == 0 || height == 0 || width > static_cast<uint32_t>(INT_MAX) ||
        height > static_cast<uint32_t>(INT_MAX) ||
        frame->stride > static_cast<uint32_t>(INT_MAX) || !cpu_data(frame)) {
        return false;
    }

    if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / 4) {
        return false;
    }
    const size_t row = static_cast<size_t>(width) * 4;
    if (frame->stride < row) return false;
    const size_t required = static_cast<size_t>(frame->stride) * height;
    if (height != 0 && required / height != frame->stride) return false;
    return frame->size >= required;
}

bool valid_audio_frame(const KopawFrame* frame) {
    if (!frame_header_valid(frame) || frame->media_type != KOPAW_MEDIA_AUDIO ||
        frame->memory_type != KOPAW_MEMORY_CPU) {
        return false;
    }
    const uint32_t rate = frame->format.audio.sample_rate;
    const uint32_t channels = frame->format.audio.channels;
    if (rate == 0 || rate > static_cast<uint32_t>(INT_MAX) || channels == 0 ||
        channels > 32 || !cpu_data(frame)) {
        return false;
    }

    const size_t bytes_per_sample = static_cast<size_t>(channels) * sizeof(float);
    if (frame->size == 0 || frame->size % bytes_per_sample != 0 ||
        frame->size > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    const size_t samples = frame->size / bytes_per_sample;
    return samples > 0 && samples <= static_cast<size_t>(INT_MAX);
}

size_t count_inout(const AVFilterInOut* list) {
    size_t count = 0;
    for (const AVFilterInOut* item = list; item; item = item->next) ++count;
    return count;
}

// Parse once without media negotiation so command-line errors are reported by
// open(), while the real graph can still be configured from the first frame.
bool validate_filter_chain(const std::string& description, const char* kind,
                           std::string* error) {
    if (description.empty()) {
        set_error(error, "滤镜链为空");
        return false;
    }

    AVFilterGraph* probe = avfilter_graph_alloc();
    if (!probe) {
        set_error(error, "avfilter_graph_alloc 失败");
        return false;
    }
    AVFilterInOut* inputs = nullptr;
    AVFilterInOut* outputs = nullptr;
    const int rc = avfilter_graph_parse2(probe, description.c_str(), &inputs, &outputs);
    const size_t input_count = count_inout(inputs);
    const size_t output_count = count_inout(outputs);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&probe);

    if (rc < 0) {
        if (error) {
            *error = std::string(kind) + "滤镜链解析失败: " + ffmpeg_error(rc);
        }
        return false;
    }
    if (input_count != 1 || output_count != 1) {
        if (error) {
            *error = std::string(kind) + "滤镜节点只支持单输入单输出链（inputs=" +
                     std::to_string(input_count) + ", outputs=" +
                     std::to_string(output_count) + ")";
        }
        return false;
    }
    return true;
}

void emit_eos(KopawGraph* graph, KopawOutput output, int32_t media_type) {
    OwnedFrame* eos = make_frame(media_type, 0, 0, 0);
    eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
    const int32_t rc = kopaw_graph_emit(graph, output, eos->ptr());
    if (rc != KOPAW_OK && rc != KOPAW_E_STOPPED) {
        KOP_LOG_WARN(kTag, "EOS 输出失败: %d", rc);
    }
}

bool is_stop_result(int32_t rc, KopawGraph* graph) {
    return rc == KOPAW_E_STOPPED ||
           (rc == KOPAW_E_EOS && graph &&
            kopaw_graph_state(graph) != KOPAW_STATE_RUNNING);
}

KopawNodeVTable make_video_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* frame) -> int32_t {
        return static_cast<VideoFilterNode*>(user)->send_impl(frame);
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<VideoFilterNode*>(user); };
    return vt;
}

KopawNodeVTable make_audio_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* frame) -> int32_t {
        return static_cast<AudioFilterNode*>(user)->send_impl(frame);
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<AudioFilterNode*>(user); };
    return vt;
}

const KopawNodeVTable kVideoVTable = make_video_vtable();
const KopawNodeVTable kAudioVTable = make_audio_vtable();

}  // namespace

VideoFilterNode::~VideoFilterNode() {
    if (in_frm_) av_frame_free(&in_frm_);
    if (out_frm_) av_frame_free(&out_frm_);
    if (fg_) avfilter_graph_free(&fg_);
}

bool VideoFilterNode::open(const std::string& graph_desc, std::string* error) {
    if (fg_ || in_frm_ || out_frm_) {
        set_error(error, "视频滤镜节点已打开");
        return false;
    }
    if (!validate_filter_chain(graph_desc, "视频", error)) return false;

    AVFrame* input = av_frame_alloc();
    AVFrame* output = av_frame_alloc();
    if (!input || !output) {
        if (input) av_frame_free(&input);
        if (output) av_frame_free(&output);
        set_error(error, "AVFrame 分配失败");
        return false;
    }
    graph_desc_ = graph_desc;
    in_frm_ = input;
    out_frm_ = output;
    configured_ = false;
    flushed_ = false;
    return true;
}

KopawNodeDesc VideoFilterNode::desc(KopawGraph* graph) {
    set_graph(graph);
    KopawNodeDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = "video_filter";
    desc.user_data = this;
    desc.outputs = 1;
    desc.inputs = 1;
    desc.queue_capacity = 8;
    desc.is_sink = 0;
    desc.self_driven = 0;
    desc.vtable = &kVideoVTable;
    return desc;
}

int32_t VideoFilterNode::configure(const KopawFrame* frame) {
    if (!valid_video_frame(frame)) return KOPAW_E_INVALID;

    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) return KOPAW_E_GENERIC;

    char args[256] = {};
    const int arg_len = snprintf(
        args, sizeof(args),
        "video_size=%ux%u:pix_fmt=rgba:time_base=1/1000000:pixel_aspect=1/1",
        frame->format.video.width, frame->format.video.height);
    if (arg_len < 0 || static_cast<size_t>(arg_len) >= sizeof(args)) {
        avfilter_graph_free(&graph);
        return KOPAW_E_INVALID;
    }

    const AVFilter* source_filter = avfilter_get_by_name("buffer");
    const AVFilter* sink_filter = avfilter_get_by_name("buffersink");
    AVFilterContext* source = source_filter
                                  ? avfilter_graph_alloc_filter(graph, source_filter, "in")
                                  : nullptr;
    AVFilterContext* sink = sink_filter
                                ? avfilter_graph_alloc_filter(graph, sink_filter, "out")
                                : nullptr;
    if (!source || !sink) {
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    int rc = avfilter_init_str(source, args);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "buffersrc 初始化失败: %s (%s)", args,
                      ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    // FFmpeg 8 uses the plural string option. Set it before sink init.
    rc = av_opt_set(sink, "pixel_formats", "rgba", AV_OPT_SEARCH_CHILDREN);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "设置视频输出格式失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    rc = avfilter_init_dict(sink, nullptr);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "buffersink 初始化失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    inputs->pad_idx = 0;
    if (!outputs->name || !inputs->name) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    rc = avfilter_graph_parse_ptr(graph, graph_desc_.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "视频滤镜图解析失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    rc = avfilter_graph_config(graph, nullptr);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "视频滤镜图 config 失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    fg_ = graph;
    src_ = source;
    sink_ = sink;
    configured_ = true;
    flushed_ = false;
    KOP_LOG_INFO(kTag, "视频滤镜图已生效: %s", graph_desc_.c_str());
    return KOPAW_OK;
}

int32_t VideoFilterNode::drain_sink(int64_t default_pts) {
    if (!sink_ || !out_frm_) return KOPAW_E_INVALID;

    while (true) {
        av_frame_unref(out_frm_);
        const int rc = av_buffersink_get_frame(sink_, out_frm_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return KOPAW_OK;
        if (rc < 0) {
            KOP_LOG_ERROR(kTag, "视频 buffersink 取帧失败: %s", ffmpeg_error(rc).c_str());
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }

        const int width = out_frm_->width;
        const int height = out_frm_->height;
        if (out_frm_->format != AV_PIX_FMT_RGBA || width <= 0 || height <= 0 ||
            !out_frm_->data[0] || out_frm_->linesize[0] <= 0) {
            KOP_LOG_ERROR(kTag, "视频 buffersink 输出帧格式无效");
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }
        const size_t row = static_cast<size_t>(width) * 4;
        if (static_cast<size_t>(out_frm_->linesize[0]) < row ||
            row > std::numeric_limits<uint32_t>::max() ||
            row > std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
            KOP_LOG_ERROR(kTag, "视频 buffersink 输出 stride 无效");
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }
        const size_t bytes = row * static_cast<size_t>(height);
        OwnedFrame* output = FramePool::acquire(KOPAW_MEDIA_VIDEO, bytes);
        for (int y = 0; y < height; ++y) {
        memcpy(output->data() + static_cast<size_t>(y) * row,
               out_frm_->data[0] + static_cast<size_t>(y) * out_frm_->linesize[0], row);
        }
        output->frame.pts = out_frm_->pts != AV_NOPTS_VALUE ? out_frm_->pts : default_pts;
        output->frame.dts = output->frame.pts;
        output->frame.format.video.width = static_cast<uint32_t>(width);
        output->frame.format.video.height = static_cast<uint32_t>(height);
        output->frame.stride = static_cast<uint32_t>(row);
        output->frame.size = bytes;
        output->frame.color = color_;
        av_frame_unref(out_frm_);

        const int32_t emit_rc = kopaw_graph_emit(g_, out_, output->ptr());
        if (emit_rc != KOPAW_OK) return emit_rc;
    }
}

int32_t VideoFilterNode::flush() {
    if (!configured_ || flushed_) return KOPAW_OK;
    const int rc = av_buffersrc_add_frame_flags(src_, nullptr, 0);
    if (rc < 0 && rc != AVERROR_EOF) {
        KOP_LOG_ERROR(kTag, "视频滤镜图冲刷失败: %s", ffmpeg_error(rc).c_str());
        return KOPAW_E_GENERIC;
    }
    flushed_ = true;
    return drain_sink(0);
}

int32_t VideoFilterNode::send_impl(KopawFrame* frame) {
    if (!frame_header_valid(frame) || !g_ || frame->media_type != KOPAW_MEDIA_VIDEO) {
        return KOPAW_E_INVALID;
    }
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        if (flushed_) return KOPAW_E_EOS;
        const int32_t flush_rc = flush();
        if (flush_rc != KOPAW_OK && flush_rc != KOPAW_E_STOPPED) {
            return flush_rc;
        }
        frame->release(frame);
        flushed_ = true;
        emit_eos(g_, out_, KOPAW_MEDIA_VIDEO);
        return KOPAW_OK;
    }
    if (flushed_) return KOPAW_E_EOS;
    if (!valid_video_frame(frame)) return KOPAW_E_INVALID;

    if (!configured_) {
        const int32_t rc = configure(frame);
        if (rc != KOPAW_OK) return rc;
    }
    color_ = frame->color;

    av_frame_unref(in_frm_);
    in_frm_->format = AV_PIX_FMT_RGBA;
    in_frm_->width = static_cast<int>(frame->format.video.width);
    in_frm_->height = static_cast<int>(frame->format.video.height);
    in_frm_->pts = frame->pts;
    const uint8_t* input = cpu_data(frame);
    if (!input || av_frame_get_buffer(in_frm_, 0) < 0 || !in_frm_->data[0] ||
        in_frm_->linesize[0] <= 0) {
        av_frame_unref(in_frm_);
        return KOPAW_E_GENERIC;
    }
    const size_t row = static_cast<size_t>(frame->format.video.width) * 4;
    if (static_cast<size_t>(in_frm_->linesize[0]) < row) {
        av_frame_unref(in_frm_);
        return KOPAW_E_GENERIC;
    }
    for (uint32_t y = 0; y < frame->format.video.height; ++y) {
        memcpy(in_frm_->data[0] + static_cast<size_t>(y) * in_frm_->linesize[0],
               input + static_cast<size_t>(y) * frame->stride, row);
    }

    const int rc = av_buffersrc_add_frame_flags(src_, in_frm_, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (rc < 0) {
        av_frame_unref(in_frm_);
        KOP_LOG_WARN(kTag, "视频 buffersrc 推帧失败: %s", ffmpeg_error(rc).c_str());
        return rc == AVERROR_EOF ? KOPAW_E_EOS : KOPAW_E_GENERIC;
    }
    // buffersrc now owns its reference; release our temporary reference and
    // then hand the original KOPAW frame back exactly once.
    const int64_t default_pts = frame->pts;
    av_frame_unref(in_frm_);
    frame->release(frame);

    const int32_t drain_rc = drain_sink(default_pts);
    if (drain_rc != KOPAW_OK && !is_stop_result(drain_rc, g_)) {
        KOP_LOG_WARN(kTag, "视频滤镜输出失败: %d", drain_rc);
    }
    // The input has already been accepted and released, so returning an
    // error here would make the graph release it a second time.
    return KOPAW_OK;
}

AudioFilterNode::~AudioFilterNode() {
    if (in_frm_) av_frame_free(&in_frm_);
    if (out_frm_) av_frame_free(&out_frm_);
    if (fg_) avfilter_graph_free(&fg_);
}

bool AudioFilterNode::open(const std::string& graph_desc, std::string* error) {
    if (fg_ || in_frm_ || out_frm_) {
        set_error(error, "音频滤镜节点已打开");
        return false;
    }
    if (!validate_filter_chain(graph_desc, "音频", error)) return false;

    AVFrame* input = av_frame_alloc();
    AVFrame* output = av_frame_alloc();
    if (!input || !output) {
        if (input) av_frame_free(&input);
        if (output) av_frame_free(&output);
        set_error(error, "AVFrame 分配失败");
        return false;
    }
    graph_desc_ = graph_desc;
    in_frm_ = input;
    out_frm_ = output;
    configured_ = false;
    flushed_ = false;
    return true;
}

KopawNodeDesc AudioFilterNode::desc(KopawGraph* graph) {
    set_graph(graph);
    KopawNodeDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.name = "audio_filter";
    desc.user_data = this;
    desc.outputs = 1;
    desc.inputs = 1;
    desc.queue_capacity = 16;
    desc.is_sink = 0;
    desc.self_driven = 0;
    desc.vtable = &kAudioVTable;
    return desc;
}

int32_t AudioFilterNode::configure(const KopawFrame* frame) {
    if (!valid_audio_frame(frame)) return KOPAW_E_INVALID;

    AVChannelLayout input_layout{};
    av_channel_layout_default(&input_layout,
                              static_cast<int>(frame->format.audio.channels));
    char layout[256] = {};
    const int layout_rc = av_channel_layout_describe(&input_layout, layout, sizeof(layout));
    av_channel_layout_uninit(&input_layout);
    if (layout_rc < 0) return KOPAW_E_GENERIC;

    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) return KOPAW_E_GENERIC;
    char args[512] = {};
    const int arg_len = snprintf(
        args, sizeof(args),
        "time_base=1/1000000:sample_rate=%u:sample_fmt=flt:channels=%u:channel_layout=%s",
        frame->format.audio.sample_rate, frame->format.audio.channels, layout);
    if (arg_len < 0 || static_cast<size_t>(arg_len) >= sizeof(args)) {
        avfilter_graph_free(&graph);
        return KOPAW_E_INVALID;
    }

    const AVFilter* source_filter = avfilter_get_by_name("abuffer");
    const AVFilter* sink_filter = avfilter_get_by_name("abuffersink");
    AVFilterContext* source = source_filter
                                  ? avfilter_graph_alloc_filter(graph, source_filter, "in")
                                  : nullptr;
    AVFilterContext* sink = sink_filter
                                ? avfilter_graph_alloc_filter(graph, sink_filter, "out")
                                : nullptr;
    if (!source || !sink) {
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    int rc = avfilter_init_str(source, args);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "abuffersrc 初始化失败: %s (%s)", args,
                      ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    rc = avfilter_init_dict(sink, nullptr);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "abuffersink 初始化失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = source;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    inputs->pad_idx = 0;
    if (!outputs->name || !inputs->name) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    // The player sink consumes f32 interleaved stereo at 48 kHz. Keep this
    // conversion inside the bridge so every audio filter chain has the same
    // KOPAW output contract.
    const std::string full_desc =
        graph_desc_ + ",aformat=sample_fmts=flt:sample_rates=48000:channel_layouts=stereo";
    rc = avfilter_graph_parse_ptr(graph, full_desc.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "音频滤镜图解析失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }
    rc = avfilter_graph_config(graph, nullptr);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "音频滤镜图 config 失败: %s", ffmpeg_error(rc).c_str());
        avfilter_graph_free(&graph);
        return KOPAW_E_GENERIC;
    }

    fg_ = graph;
    src_ = source;
    sink_ = sink;
    configured_ = true;
    flushed_ = false;
    KOP_LOG_INFO(kTag, "音频滤镜图已生效: %s", graph_desc_.c_str());
    return KOPAW_OK;
}

int32_t AudioFilterNode::drain_sink(int64_t default_pts) {
    if (!sink_ || !out_frm_) return KOPAW_E_INVALID;

    while (true) {
        av_frame_unref(out_frm_);
        const int rc = av_buffersink_get_frame(sink_, out_frm_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return KOPAW_OK;
        if (rc < 0) {
            KOP_LOG_ERROR(kTag, "音频 buffersink 取帧失败: %s", ffmpeg_error(rc).c_str());
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }

        const int channels = out_frm_->ch_layout.nb_channels;
        const int sample_rate = out_frm_->sample_rate;
        if (out_frm_->format != AV_SAMPLE_FMT_FLT || !out_frm_->data[0] ||
            channels <= 0 || sample_rate <= 0 || out_frm_->nb_samples <= 0) {
            KOP_LOG_ERROR(kTag, "音频 buffersink 输出帧格式无效");
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }
        const size_t sample_count = static_cast<size_t>(out_frm_->nb_samples);
        const size_t channel_count = static_cast<size_t>(channels);
        if (channel_count > std::numeric_limits<size_t>::max() / sizeof(float) ||
            sample_count > std::numeric_limits<size_t>::max() /
                                (channel_count * sizeof(float))) {
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }
        const size_t bytes = sample_count * channel_count * sizeof(float);
        if (out_frm_->linesize[0] < 0 ||
            static_cast<size_t>(out_frm_->linesize[0]) < bytes) {
            KOP_LOG_ERROR(kTag, "音频 buffersink 输出 buffer 大小无效");
            av_frame_unref(out_frm_);
            return KOPAW_E_GENERIC;
        }

        OwnedFrame* output = FramePool::acquire(KOPAW_MEDIA_AUDIO, bytes);
        memcpy(output->data(), out_frm_->data[0], bytes);
        output->frame.pts = out_frm_->pts != AV_NOPTS_VALUE ? out_frm_->pts : default_pts;
        output->frame.dts = output->frame.pts;
        output->frame.format.audio.sample_rate = static_cast<uint32_t>(sample_rate);
        output->frame.format.audio.channels = static_cast<uint32_t>(channels);
        output->frame.size = bytes;
        av_frame_unref(out_frm_);

        const int32_t emit_rc = kopaw_graph_emit(g_, out_, output->ptr());
        if (emit_rc != KOPAW_OK) return emit_rc;
    }
}

int32_t AudioFilterNode::flush() {
    if (!configured_ || flushed_) return KOPAW_OK;
    const int rc = av_buffersrc_add_frame_flags(src_, nullptr, 0);
    if (rc < 0 && rc != AVERROR_EOF) {
        KOP_LOG_ERROR(kTag, "音频滤镜图冲刷失败: %s", ffmpeg_error(rc).c_str());
        return KOPAW_E_GENERIC;
    }
    flushed_ = true;
    return drain_sink(0);
}

int32_t AudioFilterNode::send_impl(KopawFrame* frame) {
    if (!frame_header_valid(frame) || !g_ || frame->media_type != KOPAW_MEDIA_AUDIO) {
        return KOPAW_E_INVALID;
    }
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        if (flushed_) return KOPAW_E_EOS;
        const int32_t flush_rc = flush();
        if (flush_rc != KOPAW_OK && flush_rc != KOPAW_E_STOPPED) {
            return flush_rc;
        }
        frame->release(frame);
        flushed_ = true;
        emit_eos(g_, out_, KOPAW_MEDIA_AUDIO);
        return KOPAW_OK;
    }
    if (flushed_) return KOPAW_E_EOS;
    if (!valid_audio_frame(frame)) return KOPAW_E_INVALID;

    if (!configured_) {
        const int32_t rc = configure(frame);
        if (rc != KOPAW_OK) return rc;
    }

    av_frame_unref(in_frm_);
    in_frm_->format = AV_SAMPLE_FMT_FLT;
    in_frm_->sample_rate = static_cast<int>(frame->format.audio.sample_rate);
    in_frm_->nb_samples = static_cast<int>(
        frame->size /
        (static_cast<size_t>(frame->format.audio.channels) * sizeof(float)));
    av_channel_layout_default(&in_frm_->ch_layout,
                              static_cast<int>(frame->format.audio.channels));
    in_frm_->pts = frame->pts;
    const uint8_t* input = cpu_data(frame);
    if (!input || av_frame_get_buffer(in_frm_, 0) < 0 || !in_frm_->data[0] ||
        in_frm_->linesize[0] <= 0) {
        av_frame_unref(in_frm_);
        return KOPAW_E_GENERIC;
    }
    const size_t input_bytes = static_cast<size_t>(frame->size);
    if (static_cast<size_t>(in_frm_->linesize[0]) < input_bytes) {
        av_frame_unref(in_frm_);
        return KOPAW_E_GENERIC;
    }
    memcpy(in_frm_->data[0], input, input_bytes);

    const int rc = av_buffersrc_add_frame_flags(src_, in_frm_, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (rc < 0) {
        av_frame_unref(in_frm_);
        KOP_LOG_WARN(kTag, "音频 buffersrc 推帧失败: %s", ffmpeg_error(rc).c_str());
        return rc == AVERROR_EOF ? KOPAW_E_EOS : KOPAW_E_GENERIC;
    }
    const int64_t default_pts = frame->pts;
    av_frame_unref(in_frm_);
    frame->release(frame);

    const int32_t drain_rc = drain_sink(default_pts);
    if (drain_rc != KOPAW_OK && !is_stop_result(drain_rc, g_)) {
        KOP_LOG_WARN(kTag, "音频滤镜输出失败: %d", drain_rc);
    }
    // The original KOPAW frame was released after buffersrc accepted its copy.
    return KOPAW_OK;
}

}  // namespace kopaw
