// kopaw-transcode：KOPAW 输入、lavfi、编码和封装路径（P2.3）。
//
// 用法：kopaw-transcode <in> <out>
//   [--vc copy|编码器] [--ac copy|编码器]
//   [--filter video:<链>|audio:<链>] [--vb 2M] [--ab 192k]
//   [--duration SEC] [--timeout-ms N] [--buffer-ms N] [--opt k=v]
//
// 输入可以是本地文件、RTSP 或 HTTP(S) URL。网络 I/O 使用 FFmpeg 的
// AVIOInterruptCB，因此主动停止、读超时和远端断连不会被混成正常 EOF。

#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ffmpeg.hpp"  // FFmpeg 统一入口（外部工具经 modules include 路径引用）
#include "ffmpeg_compat.hpp"  // 跨版本编解码能力/声道布局抽象

#include "ffmpeg/io_control.hpp"
#include "kop/log.h"

static const char* kTag = "transcode";

namespace {

std::atomic<bool> g_stop{false};

constexpr size_t kMaxPrimePackets = 4096;
constexpr size_t kMaxPrimePacketBytes = 64 * 1024 * 1024;
constexpr size_t kMaxPrimeFrames = 4096;

void signal_handler(int) { g_stop.store(true, std::memory_order_release); }

struct Options {
    std::string in_url;
    std::string out_url;
    std::string video_codec = "copy";
    std::string audio_codec = "copy";
    std::string video_filter;
    std::string audio_filter;
    std::string video_bitrate = "2M";
    std::string audio_bitrate = "192k";
    double duration = 0;
    uint32_t timeout_ms = 15000;
    uint32_t buffer_ms = 250;
    std::vector<std::pair<std::string, std::string>> input_opts;
};

struct PendingPacket {
    AVPacket* packet = nullptr;
    int input_stream = -1;

    PendingPacket() = default;
    PendingPacket(AVPacket* value, int input) : packet(value), input_stream(input) {}
    PendingPacket(PendingPacket&& other) noexcept
        : packet(other.packet), input_stream(other.input_stream) {
        other.packet = nullptr;
    }
    PendingPacket& operator=(PendingPacket&& other) noexcept {
        if (this == &other) return *this;
        av_packet_free(&packet);
        packet = other.packet;
        input_stream = other.input_stream;
        other.packet = nullptr;
        return *this;
    }
    PendingPacket(const PendingPacket&) = delete;
    PendingPacket& operator=(const PendingPacket&) = delete;
    ~PendingPacket() { av_packet_free(&packet); }
};

struct StreamCtx {
    int in_idx = -1;
    int out_idx = -1;
    bool video = false;
    bool copy = false;
    std::string codec_name;
    std::string filter_desc;
    AVRational input_tb{1, 1};
    AVStream* input_stream = nullptr;
    AVStream* output_stream = nullptr;

    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    AVFrame* dec_frame = nullptr;
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* filter_src = nullptr;
    AVFilterContext* filter_sink = nullptr;
    AVFrame* filter_frame = nullptr;
    bool filter_flushed = false;
    bool decoder_flushed = false;

    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* audio_fifo = nullptr;
    int64_t next_pts = 0;
    std::vector<AVFrame*> pending_frames;

    ~StreamCtx() {
        for (AVFrame* frame : pending_frames) av_frame_free(&frame);
        av_audio_fifo_free(audio_fifo);
        swr_free(&swr);
        sws_freeContext(sws);
        av_frame_free(&filter_frame);
        avfilter_graph_free(&filter_graph);
        av_frame_free(&dec_frame);
        avcodec_free_context(&enc);
        avcodec_free_context(&dec);
    }
};

struct InputGuard {
    AVFormatContext* context = nullptr;
    ~InputGuard() { avformat_close_input(&context); }
};

struct OutputGuard {
    AVFormatContext* context = nullptr;
    bool complete = false;

    ~OutputGuard() {
        if (!context) return;
        if (complete) av_write_trailer(context);
        if (!(context->oformat->flags & AVFMT_NOFILE) && context->pb) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
};

using FrameHandler = std::function<bool(AVFrame*)>;

std::string error_string(int rc) {
    char buf[256] = {};
    av_strerror(rc, buf, sizeof(buf));
    return buf;
}

void log_error(const char* what, int rc) {
    KOP_LOG_ERROR(kTag, "%s 失败: %s", what, error_string(rc).c_str());
}

bool is_network_url(const std::string& path) {
    const auto colon = path.find(':');
    if (colon == std::string::npos) return false;
    const std::string scheme = path.substr(0, colon);
    return scheme == "rtsp" || scheme == "rtsps" || scheme == "http" ||
           scheme == "https" || scheme == "tcp" || scheme == "udp";
}

bool has_option(const std::vector<std::pair<std::string, std::string>>& opts,
                const char* key) {
    for (const auto& option : opts) {
        if (option.first == key) return true;
    }
    return false;
}

bool parse_u32(const std::string& text, uint32_t* out) {
    if (!out || text.empty()) return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || value > std::numeric_limits<uint32_t>::max()) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

int64_t parse_bitrate(const std::string& text) {
    if (text.empty()) return 0;
    char* end = nullptr;
    const double base = std::strtod(text.c_str(), &end);
    if (!end || end == text.c_str() || base <= 0) return 0;
    double multiplier = 1.0;
    if (*end == 'k' || *end == 'K') multiplier = 1000.0;
    if (*end == 'm' || *end == 'M') multiplier = 1000000.0;
    if (*end == 'g' || *end == 'G') multiplier = 1000000000.0;
    const double result = base * multiplier;
    if (result > static_cast<double>(std::numeric_limits<int64_t>::max())) return 0;
    return static_cast<int64_t>(result);
}

void usage() {
    fprintf(stderr,
            "用法: kopaw-transcode <输入> <输出> [选项]\n"
            "  --vc copy|编码器       视频流复制或编码（如 mpeg4）\n"
            "  --ac copy|编码器       音频流复制或编码（如 aac）\n"
            "  --filter SPEC          video:<链> 或 audio:<链>，可重复\n"
            "  --vb RATE              视频码率（如 2M）\n"
            "  --ab RATE              音频码率（如 192k）\n"
            "  --duration SEC         到媒体时间后正常结束\n"
            "  --timeout-ms N         网络单次 I/O 超时（0 = 不设上限）\n"
            "  --buffer-ms N          网络抖动缓冲，映射为 FFmpeg max_delay\n"
            "  --opt key=value        额外 avformat 输入选项\n");
}

bool append_filter(std::string* dst, const std::string& chain) {
    if (!dst || chain.empty()) return false;
    if (!dst->empty()) *dst += ',';
    *dst += chain;
    return true;
}

bool parse_args(int argc, char** argv, Options* options) {
    if (!options || argc < 3) return false;
    options->in_url = argv[1];
    options->out_url = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--vc") {
            options->video_codec = next();
        } else if (arg == "--ac") {
            options->audio_codec = next();
        } else if (arg == "--filter") {
            const std::string spec = next();
            if (spec.rfind("video:", 0) == 0) {
                if (!append_filter(&options->video_filter, spec.substr(6))) return false;
            } else if (spec.rfind("audio:", 0) == 0) {
                if (!append_filter(&options->audio_filter, spec.substr(6))) return false;
            } else if (!append_filter(&options->video_filter, spec)) {
                return false;
            }
        } else if (arg == "--vb") {
            options->video_bitrate = next();
        } else if (arg == "--ab") {
            options->audio_bitrate = next();
        } else if (arg == "--duration") {
            options->duration = std::atof(next().c_str());
            if (options->duration < 0) return false;
        } else if (arg == "--timeout-ms") {
            if (!parse_u32(next(), &options->timeout_ms)) return false;
        } else if (arg == "--buffer-ms") {
            if (!parse_u32(next(), &options->buffer_ms)) return false;
        } else if (arg == "--opt") {
            const std::string key_value = next();
            const auto equal = key_value.find('=');
            if (equal == std::string::npos || equal == 0) return false;
            options->input_opts.emplace_back(key_value.substr(0, equal),
                                             key_value.substr(equal + 1));
        } else {
            return false;
        }
    }
    return !options->in_url.empty() && !options->out_url.empty();
}

bool configure_filter(StreamCtx* stream, const AVFrame* frame, std::string* error) {
    if (!stream || !frame || stream->filter_desc.empty()) return true;
    if (stream->filter_graph) return true;

    stream->filter_graph = avfilter_graph_alloc();
    stream->filter_frame = av_frame_alloc();
    if (!stream->filter_graph || !stream->filter_frame) {
        if (error) *error = "分配 AVFilterGraph/AVFrame 失败";
        return false;
    }

    const AVFilter* source = nullptr;
    const AVFilter* sink = nullptr;
    char args[1024] = {};
    if (stream->video) {
        const char* pixel_format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        if (!pixel_format || frame->width <= 0 || frame->height <= 0) {
            if (error) *error = "视频帧格式无效，无法创建 lavfi 输入";
            return false;
        }
        source = avfilter_get_by_name("buffer");
        sink = avfilter_get_by_name("buffersink");
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%s:time_base=%d/%d:pixel_aspect=1/1",
                 frame->width, frame->height, pixel_format, stream->input_tb.num,
                 stream->input_tb.den);
    } else {
        source = avfilter_get_by_name("abuffer");
        sink = avfilter_get_by_name("abuffersink");
        if (!source || !sink || frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0) {
            if (error) *error = "音频帧格式无效，无法创建 lavfi 输入";
            return false;
        }
        const char* sample_format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format));
        char layout[256] = {};
        if (!sample_format || av_channel_layout_describe(&frame->ch_layout, layout,
                                                          sizeof(layout)) < 0) {
            if (error) *error = "音频采样格式或声道布局无效";
            return false;
        }
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                 stream->input_tb.num, stream->input_tb.den, frame->sample_rate,
                 sample_format, layout);
    }
    if (!source || !sink) {
        if (error) *error = "FFmpeg 缺少 buffer/buffersink 过滤器";
        return false;
    }
    stream->filter_src = avfilter_graph_alloc_filter(stream->filter_graph, source, "in");
    stream->filter_sink = avfilter_graph_alloc_filter(stream->filter_graph, sink, "out");
    if (!stream->filter_src || !stream->filter_sink ||
        avfilter_init_str(stream->filter_src, args) < 0 ||
        avfilter_init_dict(stream->filter_sink, nullptr) < 0) {
        if (error) *error = "初始化 lavfi buffer 节点失败";
        return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        if (error) *error = "分配 lavfi 连接描述失败";
        return false;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = stream->filter_src;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = stream->filter_sink;
    inputs->pad_idx = 0;
    const int parse_rc = outputs->name && inputs->name
                             ? avfilter_graph_parse_ptr(stream->filter_graph,
                                                        stream->filter_desc.c_str(), &inputs,
                                                        &outputs, nullptr)
                             : AVERROR(ENOMEM);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (parse_rc < 0 || avfilter_graph_config(stream->filter_graph, nullptr) < 0) {
        if (error) *error = "lavfi 过滤链解析或配置失败: " + stream->filter_desc;
        return false;
    }
    return true;
}

bool drain_filter(StreamCtx* stream, const FrameHandler& handler, std::string* error) {
    while (true) {
        const int rc = av_buffersink_get_frame(stream->filter_sink, stream->filter_frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            if (error) *error = "读取 lavfi 输出失败: " + error_string(rc);
            return false;
        }
        const bool accepted = handler(stream->filter_frame);
        av_frame_unref(stream->filter_frame);
        if (!accepted) return false;
    }
}

bool push_filter(StreamCtx* stream, AVFrame* frame, const FrameHandler& handler,
                 std::string* error) {
    if (stream->filter_desc.empty()) return handler(frame);
    if (!configure_filter(stream, frame, error)) return false;
    const int rc = av_buffersrc_add_frame_flags(stream->filter_src, frame,
                                                AV_BUFFERSRC_FLAG_KEEP_REF);
    if (rc < 0) {
        if (error) *error = "写入 lavfi 输入失败: " + error_string(rc);
        return false;
    }
    return drain_filter(stream, handler, error);
}

bool flush_filter(StreamCtx* stream, const FrameHandler& handler, std::string* error) {
    if (stream->filter_desc.empty() || !stream->filter_graph || stream->filter_flushed) {
        return true;
    }
    const int rc = av_buffersrc_add_frame_flags(stream->filter_src, nullptr, 0);
    if (rc < 0 && rc != AVERROR_EOF) {
        if (error) *error = "结束 lavfi 输入失败: " + error_string(rc);
        return false;
    }
    stream->filter_flushed = true;
    return drain_filter(stream, handler, error);
}

AVPixelFormat choose_pixel_format(const AVCodec* codec, AVPixelFormat preferred) {
    const AVPixelFormat* fmts = kopaw::compat::codec_pix_fmts(codec);
    if (!fmts) return preferred;
    for (const AVPixelFormat* format = fmts; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == preferred) return preferred;
    }
    return fmts[0];
}

AVSampleFormat choose_sample_format(const AVCodec* codec, AVSampleFormat preferred) {
    const AVSampleFormat* fmts = kopaw::compat::codec_sample_fmts(codec);
    if (!fmts) return preferred;
    for (const AVSampleFormat* format = fmts; *format != AV_SAMPLE_FMT_NONE;
         ++format) {
        if (*format == preferred) return preferred;
    }
    return fmts[0];
}

int choose_sample_rate(const AVCodec* codec, int preferred) {
    const int* rates = kopaw::compat::codec_sample_rates(codec);
    if (!rates) return preferred;
    for (const int* rate = rates; *rate; ++rate) {
        if (*rate == preferred) return preferred;
    }
    return rates[0];
}

bool choose_channel_layout(const AVCodec* codec, const AVChannelLayout& preferred,
                           AVChannelLayout* selected) {
    if (!selected) return false;
    if (kopaw::compat::channel_layout_copy(selected, &preferred) < 0) return false;
    const AVChannelLayout* layouts = kopaw::compat::codec_channel_layouts(codec);
    if (!layouts) return true;
    for (const AVChannelLayout* layout = layouts; layout->nb_channels; ++layout) {
        if (av_channel_layout_compare(&preferred, layout) == 0) return true;
    }
    av_channel_layout_uninit(selected);
    return av_channel_layout_copy(selected, &layouts[0]) >= 0;
}

bool create_encoder(StreamCtx* stream, AVFrame* format_frame, AVFormatContext* output,
                    const Options& options, std::string* error) {
    if (!stream || !format_frame || !output || stream->enc) return stream && stream->enc;
    const AVCodec* codec = avcodec_find_encoder_by_name(stream->codec_name.c_str());
    if (!codec) {
        if (error) *error = "找不到编码器: " + stream->codec_name;
        return false;
    }
    stream->enc = avcodec_alloc_context3(codec);
    if (!stream->enc) {
        if (error) *error = "分配编码器上下文失败";
        return false;
    }
    if (stream->video) {
        if (format_frame->width <= 0 || format_frame->height <= 0) {
            if (error) *error = "视频滤镜输出尺寸无效";
            return false;
        }
        stream->enc->width = format_frame->width;
        stream->enc->height = format_frame->height;
        stream->enc->pix_fmt = choose_pixel_format(codec, AV_PIX_FMT_YUV420P);
        stream->enc->time_base = stream->input_tb.num > 0 && stream->input_tb.den > 0
                                     ? stream->input_tb
                                     : AVRational{1, 30};
        stream->enc->bit_rate = parse_bitrate(options.video_bitrate);
        if (stream->enc->priv_data) av_opt_set(stream->enc->priv_data, "crf", "28", 0);
    } else {
        if (format_frame->sample_rate <= 0 || format_frame->ch_layout.nb_channels <= 0) {
            if (error) *error = "音频滤镜输出格式无效";
            return false;
        }
        stream->enc->sample_rate = choose_sample_rate(codec, format_frame->sample_rate);
        stream->enc->sample_fmt = choose_sample_format(codec, AV_SAMPLE_FMT_FLTP);
        if (!choose_channel_layout(codec, format_frame->ch_layout, &stream->enc->ch_layout)) {
            if (error) *error = "编码器不接受音频声道布局";
            return false;
        }
        stream->enc->time_base = AVRational{1, stream->enc->sample_rate};
        stream->enc->bit_rate = parse_bitrate(options.audio_bitrate);
    }
    if (output->oformat->flags & AVFMT_GLOBALHEADER) {
        stream->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(stream->enc, codec, nullptr) < 0) {
        if (error) *error = "打开编码器失败: " + stream->codec_name;
        return false;
    }
    stream->output_stream = avformat_new_stream(output, nullptr);
    if (!stream->output_stream ||
        avcodec_parameters_from_context(stream->output_stream->codecpar, stream->enc) < 0) {
        if (error) *error = "创建输出流参数失败";
        return false;
    }
    stream->out_idx = stream->output_stream->index;
    stream->output_stream->time_base = stream->enc->time_base;
    return true;
}

bool add_copy_stream(StreamCtx* stream, AVFormatContext* output, std::string* error) {
    stream->output_stream = avformat_new_stream(output, nullptr);
    if (!stream->output_stream ||
        avcodec_parameters_copy(stream->output_stream->codecpar,
                                stream->input_stream->codecpar) < 0) {
        if (error) *error = "创建复制输出流失败";
        return false;
    }
    stream->output_stream->codecpar->codec_tag = 0;
    stream->output_stream->time_base = stream->input_stream->time_base;
    stream->out_idx = stream->output_stream->index;
    return true;
}

bool write_packet(AVFormatContext* output, AVPacket* packet, kopaw::FfmpegIoControl* io,
                  bool network, const Options& options, std::string* error) {
    if (io) io->begin(network ? options.timeout_ms : 0);
    const int rc = av_interleaved_write_frame(output, packet);
    const bool timed_out = io && io->was_timed_out();
    if (io) io->end();
    if (rc < 0) {
        if (error) {
            *error = timed_out ? "网络输出超时: " : "写输出包失败: ";
            *error += error_string(rc);
        }
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kop::log_set_level(kop::log_level_from_env());
    Options options;
    if (!parse_args(argc, argv, &options)) {
        usage();
        return 2;
    }
    if ((!options.video_filter.empty() && options.video_codec == "copy") ||
        (!options.audio_filter.empty() && options.audio_codec == "copy")) {
        KOP_LOG_ERROR(kTag, "lavfi 过滤器要求对应流解码并重新编码，不能与 copy 同时使用");
        return 2;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    avformat_network_init();

    const bool input_network = is_network_url(options.in_url);
    const bool output_network = is_network_url(options.out_url);
    kopaw::FfmpegIoControl input_io{&g_stop};
    kopaw::FfmpegIoControl output_io{&g_stop};
    InputGuard input;
    OutputGuard output;
    AVDictionary* input_dict = nullptr;
    for (const auto& option : options.input_opts) {
        av_dict_set(&input_dict, option.first.c_str(), option.second.c_str(), 0);
    }
    if (input_network && options.timeout_ms > 0 && !has_option(options.input_opts, "rw_timeout")) {
        av_dict_set_int(&input_dict, "rw_timeout",
                        static_cast<int64_t>(options.timeout_ms) * 1000, 0);
    }
    if (input_network && options.buffer_ms > 0 && !has_option(options.input_opts, "max_delay")) {
        av_dict_set_int(&input_dict, "max_delay",
                        static_cast<int64_t>(options.buffer_ms) * 1000, 0);
    }

    input.context = avformat_alloc_context();
    if (!input.context) {
        KOP_LOG_ERROR(kTag, "分配输入上下文失败");
        av_dict_free(&input_dict);
        return 1;
    }
    input.context->interrupt_callback.callback = &kopaw::ffmpeg_interrupt_callback;
    input.context->interrupt_callback.opaque = &input_io;
    input_io.begin(input_network ? options.timeout_ms : 0);
    int rc = avformat_open_input(&input.context, options.in_url.c_str(), nullptr, &input_dict);
    const bool open_timed_out = input_io.was_timed_out();
    input_io.end();
    av_dict_free(&input_dict);
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "%s: %s", open_timed_out ? "打开网络输入超时" : "打开输入失败",
                      error_string(rc).c_str());
        return 1;
    }
    input_io.begin(input_network ? options.timeout_ms : 0);
    rc = avformat_find_stream_info(input.context, nullptr);
    const bool probe_timed_out = input_io.was_timed_out();
    input_io.end();
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "%s: %s", probe_timed_out ? "探测网络流超时" : "探测流信息失败",
                      error_string(rc).c_str());
        return 1;
    }

    const int selected_video =
        av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int selected_audio =
        av_find_best_stream(input.context, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (selected_video < 0 && selected_audio < 0) {
        KOP_LOG_ERROR(kTag, "输入中没有可处理的音视频流");
        return 1;
    }

    rc = avformat_alloc_output_context2(&output.context, nullptr, nullptr,
                                        options.out_url.c_str());
    if (rc < 0 || !output.context) {
        log_error("创建输出上下文", rc < 0 ? rc : AVERROR(EINVAL));
        return 1;
    }
    output.context->interrupt_callback.callback = &kopaw::ffmpeg_interrupt_callback;
    output.context->interrupt_callback.opaque = &output_io;

    StreamCtx streams[2];  // 0=video, 1=audio
    const int selected[2] = {selected_video, selected_audio};
    const std::string codecs[2] = {options.video_codec, options.audio_codec};
    const std::string filters[2] = {options.video_filter, options.audio_filter};
    int stream_count = 0;
    for (int index = 0; index < 2; ++index) {
        if (selected[index] < 0) continue;
        StreamCtx& stream = streams[index];
        stream.in_idx = selected[index];
        stream.video = index == 0;
        stream.codec_name = codecs[index];
        stream.filter_desc = filters[index];
        stream.input_stream = input.context->streams[stream.in_idx];
        stream.input_tb = stream.input_stream->time_base;
        if (stream.copy) {
            // copy is assigned below after validating the selected codec name.
        }
        stream.copy = stream.codec_name == "copy";
        if (stream.copy) {
            if (!add_copy_stream(&stream, output.context, nullptr)) {
                KOP_LOG_ERROR(kTag, "创建复制输出流失败");
                return 1;
            }
            ++stream_count;
            continue;
        }
        const AVCodec* decoder =
            avcodec_find_decoder(stream.input_stream->codecpar->codec_id);
        stream.dec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        if (!decoder || !stream.dec ||
            avcodec_parameters_to_context(stream.dec, stream.input_stream->codecpar) < 0 ||
            avcodec_open2(stream.dec, decoder, nullptr) < 0) {
            KOP_LOG_ERROR(kTag, "打开%s解码器失败", stream.video ? "视频" : "音频");
            return 1;
        }
        stream.dec->pkt_timebase = stream.input_tb;
        stream.dec_frame = av_frame_alloc();
        if (!stream.dec_frame) {
            KOP_LOG_ERROR(kTag, "分配解码帧失败");
            return 1;
        }
        if (stream.filter_desc.empty()) {
            AVFrame format_frame{};
            format_frame.width = stream.dec->width;
            format_frame.height = stream.dec->height;
            format_frame.format = stream.dec->pix_fmt == AV_PIX_FMT_NONE
                                      ? AV_PIX_FMT_YUV420P
                                      : stream.dec->pix_fmt;
            format_frame.sample_rate = stream.dec->sample_rate;
            if (stream.video) {
                // no-op: video fields above are the complete format description
            } else if (stream.dec->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(&format_frame.ch_layout, &stream.dec->ch_layout);
            } else {
                av_channel_layout_default(
                    &format_frame.ch_layout,
                    stream.dec->ch_layout.nb_channels > 0 ? stream.dec->ch_layout.nb_channels : 2);
            }
            std::string error;
            const bool created = create_encoder(&stream, &format_frame, output.context,
                                                options, &error);
            av_channel_layout_uninit(&format_frame.ch_layout);
            if (!created) {
                KOP_LOG_ERROR(kTag, "%s", error.c_str());
                return 1;
            }
        }
        ++stream_count;
    }
    if (stream_count == 0) {
        KOP_LOG_ERROR(kTag, "没有可处理的音视频流");
        return 1;
    }

    bool needs_prime = false;
    for (int index = 0; index < 2; ++index) {
        if (!streams[index].copy && !streams[index].filter_desc.empty()) needs_prime = true;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        KOP_LOG_ERROR(kTag, "分配输入包失败");
        return 1;
    }
    std::vector<PendingPacket> pending_packets;
    size_t pending_packet_bytes = 0;
    size_t pending_frame_count = 0;
    int64_t max_input_us = 0;
    const int64_t end_us = options.duration > 0
                               ? static_cast<int64_t>(options.duration * 1000000.0)
                               : std::numeric_limits<int64_t>::max();

    auto read_packet = [&]() -> int {
        int64_t eagain_since = 0;
        const int64_t timeout_us = static_cast<int64_t>(options.timeout_ms) * 1000;
        while (!g_stop.load(std::memory_order_acquire)) {
            const int64_t read_deadline =
                input_network && timeout_us > 0
                    ? (eagain_since == 0 ? kopaw::FfmpegIoControl::now_us() + timeout_us
                                         : eagain_since + timeout_us)
                    : 0;
            input_io.begin_until(read_deadline);
            const int read_rc = av_read_frame(input.context, packet);
            const bool timed_out = input_io.was_timed_out();
            input_io.end();
            if (read_rc != AVERROR(EAGAIN)) {
                if (timed_out && !g_stop.load(std::memory_order_acquire)) {
                    return AVERROR(ETIMEDOUT);
                }
                return read_rc;
            }
            if (!input_network) return read_rc;
            if (eagain_since == 0) eagain_since = kopaw::FfmpegIoControl::now_us();
            if (timed_out ||
                (input_network && options.timeout_ms > 0 &&
                 kopaw::FfmpegIoControl::now_us() - eagain_since >=
                     static_cast<int64_t>(options.timeout_ms) * 1000)) {
                input_io.timed_out.store(true, std::memory_order_release);
                return AVERROR(ETIMEDOUT);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return AVERROR_EXIT;
    };

    std::function<bool(StreamCtx&, AVFrame*, bool)> process_frame;
    std::function<bool(StreamCtx&, bool)> flush_decoder;
    std::string pipeline_error;

    auto write_encoded = [&](StreamCtx& stream, AVPacket* encoded) -> bool {
        av_packet_rescale_ts(encoded, stream.enc->time_base, stream.output_stream->time_base);
        encoded->stream_index = stream.out_idx;
        return write_packet(output.context, encoded, &output_io, output_network, options,
                            &pipeline_error);
    };

    // 只负责把一个已经准备好的 AVFrame 送入编码器并拉取输出。
    // 音频 FIFO 的排空必须调用这一层，不能再次进入 encode_frame。
    auto send_encoded_frame = [&](StreamCtx& stream, AVFrame* frame) -> bool {
        const int send_rc = avcodec_send_frame(stream.enc, frame);
        if (send_rc < 0) {
            pipeline_error = "写入编码器失败: " + error_string(send_rc);
            return false;
        }
        AVPacket* encoded = av_packet_alloc();
        if (!encoded) {
            pipeline_error = "分配编码输出包失败";
            return false;
        }
        bool ok = true;
        while (ok) {
            const int receive_rc = avcodec_receive_packet(stream.enc, encoded);
            if (receive_rc == AVERROR(EAGAIN) || receive_rc == AVERROR_EOF) break;
            if (receive_rc < 0) {
                pipeline_error = "读取编码器输出失败: " + error_string(receive_rc);
                ok = false;
                break;
            }
            ok = write_encoded(stream, encoded);
            av_packet_unref(encoded);
        }
        av_packet_free(&encoded);
        return ok;
    };

    std::function<bool(StreamCtx&, AVFrame*)> encode_frame;
    auto drain_encoder = [&](StreamCtx& stream) -> bool {
        if (!stream.enc) return true;
        const int send_rc = avcodec_send_frame(stream.enc, nullptr);
        if (send_rc < 0 && send_rc != AVERROR_EOF) {
            pipeline_error = "结束编码器失败: " + error_string(send_rc);
            return false;
        }
        AVPacket* encoded = av_packet_alloc();
        if (!encoded) {
            pipeline_error = "分配编码输出包失败";
            return false;
        }
        bool ok = true;
        while (ok) {
            const int receive_rc = avcodec_receive_packet(stream.enc, encoded);
            if (receive_rc == AVERROR(EAGAIN) || receive_rc == AVERROR_EOF) break;
            if (receive_rc < 0) {
                pipeline_error = "读取编码器输出失败: " + error_string(receive_rc);
                ok = false;
                break;
            }
            ok = write_encoded(stream, encoded);
            av_packet_unref(encoded);
        }
        av_packet_free(&encoded);
        return ok;
    };

    auto drain_audio_fifo = [&](StreamCtx& stream, bool flush) -> bool {
        const int fixed_samples = stream.enc->frame_size > 0 ? stream.enc->frame_size : 0;
        while (true) {
            const int available = av_audio_fifo_size(stream.audio_fifo);
            if (available <= 0) return true;
            if (!flush && fixed_samples > 0 && available < fixed_samples) return true;
            const int target = fixed_samples > 0 ? fixed_samples : available;
            AVFrame* audio = av_frame_alloc();
            if (!audio) {
                pipeline_error = "分配编码音频帧失败";
                return false;
            }
            audio->format = stream.enc->sample_fmt;
            audio->sample_rate = stream.enc->sample_rate;
            audio->nb_samples = target;
            if (av_channel_layout_copy(&audio->ch_layout, &stream.enc->ch_layout) < 0 ||
                av_frame_get_buffer(audio, 0) < 0) {
                av_frame_free(&audio);
                pipeline_error = "分配编码音频缓冲失败";
                return false;
            }
            const int read = av_audio_fifo_read(
                stream.audio_fifo, reinterpret_cast<void**>(audio->extended_data), target);
            if (read < 0) {
                av_frame_free(&audio);
                pipeline_error = "读取音频缓冲失败";
                return false;
            }
            if (read < target) {
                av_samples_set_silence(audio->extended_data, read, target - read,
                                        stream.enc->ch_layout.nb_channels,
                                        stream.enc->sample_fmt);
            }
            audio->pts = stream.next_pts;
            stream.next_pts += target;
            const bool ok = send_encoded_frame(stream, audio);
            av_frame_free(&audio);
            if (!ok) return false;
            if (!flush && fixed_samples == 0) return true;
        }
    };

    auto ensure_audio_converter = [&](StreamCtx& stream, const AVFrame* frame) -> bool {
        if (stream.swr) return true;
        AVChannelLayout input_layout{};
        if (av_channel_layout_copy(&input_layout, &frame->ch_layout) < 0) {
            pipeline_error = "复制输入音频声道布局失败";
            return false;
        }
        const int rc_swr = swr_alloc_set_opts2(
            &stream.swr, &stream.enc->ch_layout, stream.enc->sample_fmt,
            stream.enc->sample_rate, &input_layout,
            static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&input_layout);
        if (rc_swr < 0 || !stream.swr || swr_init(stream.swr) < 0) {
            pipeline_error = "初始化音频格式转换失败";
            return false;
        }
        stream.audio_fifo = av_audio_fifo_alloc(stream.enc->sample_fmt,
                                                stream.enc->ch_layout.nb_channels, 1);
        if (!stream.audio_fifo) {
            pipeline_error = "分配音频缓冲失败";
            return false;
        }
        return true;
    };

    encode_frame = [&](StreamCtx& stream, AVFrame* frame) -> bool {
        if (stream.video) {
            if (!stream.sws) {
                stream.sws = sws_getContext(
                    frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                    stream.enc->width, stream.enc->height, stream.enc->pix_fmt, SWS_BILINEAR,
                    nullptr, nullptr, nullptr);
                if (!stream.sws) {
                    pipeline_error = "创建视频格式转换失败";
                    return false;
                }
            }
            AVFrame* video = av_frame_alloc();
            if (!video) {
                pipeline_error = "分配编码视频帧失败";
                return false;
            }
            video->format = stream.enc->pix_fmt;
            video->width = stream.enc->width;
            video->height = stream.enc->height;
            if (av_frame_get_buffer(video, 32) < 0) {
                av_frame_free(&video);
                pipeline_error = "分配编码视频缓冲失败";
                return false;
            }
            sws_scale(stream.sws, frame->data, frame->linesize, 0, frame->height, video->data,
                      video->linesize);
            video->pts = frame->pts == AV_NOPTS_VALUE
                             ? stream.next_pts++
                             : av_rescale_q(frame->pts, stream.input_tb,
                                            stream.enc->time_base);
            const bool ok = send_encoded_frame(stream, video);
            av_frame_free(&video);
            if (!ok) return false;
        } else {
            if (!ensure_audio_converter(stream, frame)) return false;
            const int capacity = std::max(1, swr_get_out_samples(stream.swr, frame->nb_samples));
            AVFrame* converted = av_frame_alloc();
            if (!converted) {
                pipeline_error = "分配转换音频帧失败";
                return false;
            }
            converted->format = stream.enc->sample_fmt;
            converted->sample_rate = stream.enc->sample_rate;
            converted->nb_samples = capacity;
            if (av_channel_layout_copy(&converted->ch_layout, &stream.enc->ch_layout) < 0 ||
                av_frame_get_buffer(converted, 0) < 0) {
                av_frame_free(&converted);
                pipeline_error = "分配转换音频缓冲失败";
                return false;
            }
            const int produced = swr_convert(
                stream.swr, converted->extended_data, capacity,
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            if (produced < 0 ||
                av_audio_fifo_write(stream.audio_fifo,
                                    reinterpret_cast<void**>(converted->extended_data), produced) <
                    produced) {
                av_frame_free(&converted);
                pipeline_error = "写入音频缓冲失败";
                return false;
            }
            av_frame_free(&converted);
            return drain_audio_fifo(stream, false);
        }

        return true;
    };

    auto encode_pending_audio_and_flush = [&](StreamCtx& stream) -> bool {
        if (!stream.video && stream.audio_fifo) {
            while (true) {
                const int delayed = swr_get_delay(stream.swr, stream.enc->sample_rate);
                if (delayed <= 0) break;
                const int capacity = std::max(1, delayed + 32);
                AVFrame* converted = av_frame_alloc();
                if (!converted) {
                    pipeline_error = "分配音频冲刷帧失败";
                    return false;
                }
                converted->format = stream.enc->sample_fmt;
                converted->sample_rate = stream.enc->sample_rate;
                converted->nb_samples = capacity;
                if (av_channel_layout_copy(&converted->ch_layout, &stream.enc->ch_layout) < 0 ||
                    av_frame_get_buffer(converted, 0) < 0) {
                    av_frame_free(&converted);
                    pipeline_error = "分配音频冲刷缓冲失败";
                    return false;
                }
                const int produced = swr_convert(stream.swr, converted->extended_data,
                                                  capacity, nullptr, 0);
                if (produced <= 0) {
                    av_frame_free(&converted);
                    break;
                }
                if (av_audio_fifo_write(
                        stream.audio_fifo, reinterpret_cast<void**>(converted->extended_data),
                        produced) < produced) {
                    av_frame_free(&converted);
                    pipeline_error = "写入音频冲刷数据失败";
                    return false;
                }
                av_frame_free(&converted);
            }
            if (!drain_audio_fifo(stream, true)) return false;
        }
        return drain_encoder(stream);
    };

    process_frame = [&](StreamCtx& stream, AVFrame* frame, bool priming) -> bool {
        return push_filter(
            &stream, frame,
            [&](AVFrame* filtered) -> bool {
                if (!stream.enc) {
                    if (!create_encoder(&stream, filtered, output.context, options,
                                        &pipeline_error)) {
                        return false;
                    }
                }
                if (priming) {
                    if (pending_frame_count >= kMaxPrimeFrames) {
                        pipeline_error = "滤镜参数协商暂存的帧超过上限";
                        return false;
                    }
                    AVFrame* saved = av_frame_clone(filtered);
                    if (!saved) {
                        pipeline_error = "保存首轮滤镜输出失败";
                        return false;
                    }
                    stream.pending_frames.push_back(saved);
                    ++pending_frame_count;
                    return true;
                }
                return encode_frame(stream, filtered);
            },
            &pipeline_error);
    };

    auto decode_packet = [&](StreamCtx& stream, AVPacket* input_packet, bool priming) -> bool {
        const int send_rc = avcodec_send_packet(stream.dec, input_packet);
        if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) {
            pipeline_error = "写入解码器失败: " + error_string(send_rc);
            return false;
        }
        while (true) {
            const int receive_rc = avcodec_receive_frame(stream.dec, stream.dec_frame);
            if (receive_rc == AVERROR(EAGAIN) || receive_rc == AVERROR_EOF) break;
            if (receive_rc < 0) {
                pipeline_error = "读取解码器输出失败: " + error_string(receive_rc);
                return false;
            }
            if (!process_frame(stream, stream.dec_frame, priming)) return false;
            av_frame_unref(stream.dec_frame);
        }
        return true;
    };

    flush_decoder = [&](StreamCtx& stream, bool priming) -> bool {
        if (stream.in_idx < 0 || stream.copy || stream.decoder_flushed) return true;
        const int send_rc = avcodec_send_packet(stream.dec, nullptr);
        if (send_rc < 0 && send_rc != AVERROR_EOF) {
            pipeline_error = "冲刷解码器失败: " + error_string(send_rc);
            return false;
        }
        while (true) {
            const int receive_rc = avcodec_receive_frame(stream.dec, stream.dec_frame);
            if (receive_rc == AVERROR(EAGAIN) || receive_rc == AVERROR_EOF) break;
            if (receive_rc < 0) {
                pipeline_error = "读取冲刷解码帧失败: " + error_string(receive_rc);
                return false;
            }
            if (!process_frame(stream, stream.dec_frame, priming)) return false;
            av_frame_unref(stream.dec_frame);
        }
        if (!flush_filter(&stream,
                          [&](AVFrame* filtered) -> bool {
                              if (!stream.enc) {
                                  if (!create_encoder(&stream, filtered, output.context, options,
                                                      &pipeline_error)) {
                                      return false;
                                  }
                              }
                              if (priming) {
                                  if (pending_frame_count >= kMaxPrimeFrames) {
                                      pipeline_error = "滤镜冲刷暂存的帧超过上限";
                                      return false;
                                  }
                                  AVFrame* saved = av_frame_clone(filtered);
                                  if (!saved) {
                                      pipeline_error = "保存冲刷滤镜输出失败";
                                      return false;
                                  }
                                  stream.pending_frames.push_back(saved);
                                  ++pending_frame_count;
                                  return true;
                              }
                              return encode_frame(stream, filtered);
                          },
                          &pipeline_error)) {
            return false;
        }
        stream.decoder_flushed = true;
        return true;
    };

    auto all_prime_streams_ready = [&]() {
        for (int index = 0; index < 2; ++index) {
            if (streams[index].in_idx >= 0 && !streams[index].copy && !streams[index].enc)
                return false;
        }
        return true;
    };

    bool normal_end = false;
    if (needs_prime) {
        while (!all_prime_streams_ready()) {
            rc = read_packet();
            if (rc == AVERROR_EOF) {
                for (int index = 0; index < 2; ++index) {
                    if (!flush_decoder(streams[index], true)) break;
                }
                normal_end = true;
                break;
            }
            if (rc < 0) {
                KOP_LOG_ERROR(kTag, "%s: %s", rc == AVERROR(ETIMEDOUT)
                                                    ? "网络输入超时"
                                                    : "网络输入断连或读取失败",
                              error_string(rc).c_str());
                av_packet_free(&packet);
                return rc == AVERROR_EXIT ? 130 : 1;
            }
            AVStream* input_stream = input.context->streams[packet->stream_index];
            const int64_t raw_pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            const int64_t pts_us = raw_pts == AV_NOPTS_VALUE
                                       ? -1
                                       : av_rescale_q(raw_pts, input_stream->time_base,
                                                      AVRational{1, 1000000});
            if (pts_us >= 0) max_input_us = std::max(max_input_us, pts_us);
            if (pts_us >= end_us) {
                av_packet_unref(packet);
                normal_end = true;
                break;
            }
            StreamCtx* stream = nullptr;
            if (packet->stream_index == streams[0].in_idx) stream = &streams[0];
            if (packet->stream_index == streams[1].in_idx) stream = &streams[1];
            if (stream && stream->copy) {
                const size_t packet_bytes = packet->size > 0
                                                ? static_cast<size_t>(packet->size)
                                                : 0;
                if (pending_packets.size() >= kMaxPrimePackets ||
                    packet_bytes > kMaxPrimePacketBytes ||
                    pending_packet_bytes > kMaxPrimePacketBytes - packet_bytes) {
                    KOP_LOG_ERROR(kTag,
                                  "滤镜参数协商暂存的复制包超过上限（最多 %zu 包/%zu MiB）",
                                  kMaxPrimePackets, kMaxPrimePacketBytes / (1024 * 1024));
                    av_packet_free(&packet);
                    return 1;
                }
                AVPacket* saved = av_packet_clone(packet);
                if (!saved) {
                    KOP_LOG_ERROR(kTag, "保存首轮复制包失败");
                    av_packet_free(&packet);
                    return 1;
                }
                pending_packets.push_back(PendingPacket{saved, packet->stream_index});
                pending_packet_bytes += packet_bytes;
            } else if (stream) {
                if (!decode_packet(*stream, packet, true)) {
                    KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
                    av_packet_free(&packet);
                    return 1;
                }
            }
            av_packet_unref(packet);
        }
        if (!all_prime_streams_ready()) {
            KOP_LOG_ERROR(kTag, "滤镜链在输入结束前没有产生可编码帧: %s",
                          pipeline_error.c_str());
            av_packet_free(&packet);
            return 1;
        }
    }

    for (int index = 0; index < 2; ++index) {
        if (!streams[index].copy && !streams[index].filter_desc.empty() && !streams[index].enc) {
            KOP_LOG_ERROR(kTag, "无法协商%s滤镜输出编码参数", streams[index].video ? "视频" : "音频");
            av_packet_free(&packet);
            return 1;
        }
    }

    if (!(output.context->oformat->flags & AVFMT_NOFILE)) {
        output_io.begin(output_network ? options.timeout_ms : 0);
        rc = avio_open(&output.context->pb, options.out_url.c_str(), AVIO_FLAG_WRITE);
        const bool timed_out = output_io.was_timed_out();
        output_io.end();
        if (rc < 0) {
            KOP_LOG_ERROR(kTag, "%s: %s", timed_out ? "打开网络输出超时" : "打开输出 IO 失败",
                          error_string(rc).c_str());
            av_packet_free(&packet);
            return 1;
        }
    }
    output_io.begin(output_network ? options.timeout_ms : 0);
    rc = avformat_write_header(output.context, nullptr);
    const bool header_timed_out = output_io.was_timed_out();
    output_io.end();
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "%s: %s", header_timed_out ? "写输出头超时" : "写输出头失败",
                      error_string(rc).c_str());
        av_packet_free(&packet);
        return 1;
    }
    KOP_LOG_INFO(kTag, "%s -> %s（视频=%s，音频=%s，缓冲=%ums）", options.in_url.c_str(),
                 options.out_url.c_str(), options.video_codec.c_str(), options.audio_codec.c_str(),
                 options.buffer_ms);

    // 首轮为滤镜输出协商编码参数时暂存的帧/复制包现在可以写入容器。
    for (PendingPacket& pending : pending_packets) {
        StreamCtx* stream = nullptr;
        if (pending.input_stream == streams[0].in_idx) stream = &streams[0];
        if (pending.input_stream == streams[1].in_idx) stream = &streams[1];
        if (!stream || !stream->copy) continue;
        av_packet_rescale_ts(pending.packet, stream->input_stream->time_base,
                             stream->output_stream->time_base);
        pending.packet->stream_index = stream->out_idx;
        if (!write_packet(output.context, pending.packet, &output_io, output_network, options,
                          &pipeline_error)) {
            KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
            av_packet_free(&packet);
            return 1;
        }
    }
    pending_packets.clear();
    pending_packet_bytes = 0;
    for (int index = 0; index < 2; ++index) {
        StreamCtx& stream = streams[index];
        for (AVFrame* pending : stream.pending_frames) {
            if (!encode_frame(stream, pending)) {
                KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
                av_packet_free(&packet);
                return 1;
            }
            av_frame_free(&pending);
        }
        stream.pending_frames.clear();
    }
    pending_frame_count = 0;

    while (!normal_end) {
        rc = read_packet();
        if (rc == AVERROR_EOF) {
            normal_end = true;
            break;
        }
        if (rc < 0) {
            KOP_LOG_ERROR(kTag, "%s: %s", rc == AVERROR(ETIMEDOUT)
                                                ? "网络输入超时"
                                                : (rc == AVERROR_EXIT ? "转码被停止"
                                                                      : "网络输入断连或读取失败"),
                          error_string(rc).c_str());
            av_packet_free(&packet);
            return rc == AVERROR_EXIT ? 130 : 1;
        }
        AVStream* input_stream = input.context->streams[packet->stream_index];
        const int64_t raw_pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        const int64_t pts_us = raw_pts == AV_NOPTS_VALUE
                                   ? -1
                                   : av_rescale_q(raw_pts, input_stream->time_base,
                                                  AVRational{1, 1000000});
        if (pts_us >= 0) max_input_us = std::max(max_input_us, pts_us);
        if (pts_us >= end_us) {
            av_packet_unref(packet);
            normal_end = true;
            break;
        }
        StreamCtx* stream = nullptr;
        if (packet->stream_index == streams[0].in_idx) stream = &streams[0];
        if (packet->stream_index == streams[1].in_idx) stream = &streams[1];
        if (stream) {
            if (stream->copy) {
                av_packet_rescale_ts(packet, stream->input_stream->time_base,
                                     stream->output_stream->time_base);
                packet->stream_index = stream->out_idx;
                if (!write_packet(output.context, packet, &output_io, output_network, options,
                                  &pipeline_error)) {
                    KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
                    av_packet_unref(packet);
                    av_packet_free(&packet);
                    return 1;
                }
            } else if (!decode_packet(*stream, packet, false)) {
                KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
                av_packet_unref(packet);
                av_packet_free(&packet);
                return 1;
            }
        }
        av_packet_unref(packet);
    }

    if (g_stop.load(std::memory_order_acquire)) {
        KOP_LOG_INFO(kTag, "收到停止信号，放弃未完成输出");
        av_packet_free(&packet);
        return 130;
    }
    for (int index = 0; index < 2; ++index) {
        StreamCtx& stream = streams[index];
        if (stream.copy) continue;
        if (!flush_decoder(stream, false) || !encode_pending_audio_and_flush(stream)) {
            KOP_LOG_ERROR(kTag, "%s", pipeline_error.c_str());
            av_packet_free(&packet);
            return 1;
        }
    }
    av_packet_free(&packet);

    output_io.begin(output_network ? options.timeout_ms : 0);
    rc = av_write_trailer(output.context);
    const bool trailer_timed_out = output_io.was_timed_out();
    output_io.end();
    if (rc < 0) {
        KOP_LOG_ERROR(kTag, "%s: %s", trailer_timed_out ? "写输出尾部超时" : "写输出尾部失败",
                      error_string(rc).c_str());
        return 1;
    }
    output.complete = false;  // trailer has already been written explicitly
    KOP_LOG_INFO(kTag, "完成（约 %.1fs 媒体时间）", max_input_us / 1e6);
    return 0;
}
