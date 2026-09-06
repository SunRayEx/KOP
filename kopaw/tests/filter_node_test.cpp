#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "ffmpeg/filter_node.hpp"
#include "frame.hpp"
#include "kopaw_abi.h"

namespace {

enum class SourceKind { Video, Audio };

struct SourceState {
    KopawGraph* graph = nullptr;
    KopawOutput output{};
    SourceKind kind = SourceKind::Video;
    uint32_t audio_rate = 48'000;
    uint32_t audio_channels = 2;
    uint32_t audio_samples = 480;
    float audio_value = 1.0f;
};

struct SinkState {
    KopawGraph* graph = nullptr;
    uint32_t node_id = 0;
    int32_t expected_media = KOPAW_MEDIA_VIDEO;
    std::atomic<int> data_frames{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<uint32_t> sample_rate{0};
    std::atomic<uint32_t> channels{0};
    std::atomic<uint32_t> samples{0};
    std::atomic<int> first_sample_milli{0};
    std::atomic<uint32_t> first_pixel{0};
    std::atomic<bool> grayscale{false};
    std::atomic<bool> bad_frame{false};
};

int32_t source_run(void* user) {
    auto* source = static_cast<SourceState*>(user);
    if (source->kind == SourceKind::Video) {
        kopaw::OwnedFrame* frame = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 4 * 2 * 4);
        frame->frame.format.video.width = 4;
        frame->frame.format.video.height = 2;
        frame->frame.stride = 16;
        const uint8_t pixels[8][4] = {
            {255, 0, 0, 255},   {0, 255, 0, 255},   {0, 0, 255, 255},
            {255, 255, 255, 255}, {0, 0, 0, 255},   {255, 255, 0, 255},
            {0, 255, 255, 255}, {255, 0, 255, 255},
        };
        memcpy(frame->data(), pixels, sizeof(pixels));
        kopaw_graph_emit(source->graph, source->output, frame->ptr());
        kopaw::OwnedFrame* eos = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 33'333, 33'333, 0);
        eos->frame.flags = KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(source->graph, source->output, eos->ptr());
    } else {
        const uint32_t rate = source->audio_rate;
        const uint32_t channels = source->audio_channels;
        const uint32_t sample_count = source->audio_samples;
        kopaw::OwnedFrame* frame = kopaw::make_frame(
            KOPAW_MEDIA_AUDIO, 0, 0,
            static_cast<size_t>(sample_count) * channels * sizeof(float));
        frame->frame.format.audio.sample_rate = rate;
        frame->frame.format.audio.channels = channels;
        auto* sample_data = reinterpret_cast<float*>(frame->data());
        for (uint32_t i = 0; i < source->audio_samples * source->audio_channels; ++i)
            sample_data[i] = source->audio_value;
        kopaw_graph_emit(source->graph, source->output, frame->ptr());
        const int64_t duration_us = static_cast<int64_t>(source->audio_samples) * 1'000'000 /
                                    source->audio_rate;
        kopaw::OwnedFrame* eos = kopaw::make_frame(
            KOPAW_MEDIA_AUDIO, duration_us, duration_us, 0);
        eos->frame.flags = KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(source->graph, source->output, eos->ptr());
    }
    return KOPAW_OK;
}

int32_t sink_send(void* user, KopawFrame* frame) {
    auto* sink = static_cast<SinkState*>(user);
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        frame->release(frame);
        return kopaw_node_sink_done(sink->graph, sink->node_id);
    }
    if (frame->media_type != sink->expected_media || frame->struct_size < sizeof(KopawFrame)) {
        sink->bad_frame.store(true);
    } else if (frame->media_type == KOPAW_MEDIA_VIDEO) {
        sink->width.store(frame->format.video.width);
        sink->height.store(frame->format.video.height);
        const size_t row = static_cast<size_t>(frame->format.video.width) * 4;
        const size_t required = static_cast<size_t>(frame->stride) * frame->format.video.height;
        const uint8_t* input = kopaw::cpu_data(frame);
        if (!input || frame->stride < row || frame->size < required) {
            sink->bad_frame.store(true);
        } else {
            const uint8_t* first = input;
            sink->first_pixel.store(static_cast<uint32_t>(first[0]) |
                                    (static_cast<uint32_t>(first[1]) << 8) |
                                    (static_cast<uint32_t>(first[2]) << 16) |
                                    (static_cast<uint32_t>(first[3]) << 24));
            bool gray = true;
            for (uint32_t y = 0; y < frame->format.video.height && gray; ++y) {
                const uint8_t* line = input + static_cast<size_t>(y) * frame->stride;
                for (uint32_t x = 0; x < frame->format.video.width; ++x) {
                    const uint8_t* pixel = line + static_cast<size_t>(x) * 4;
                    if (pixel[0] != pixel[1] || pixel[1] != pixel[2]) {
                        gray = false;
                        break;
                    }
                }
            }
            sink->grayscale.store(gray);
        }
        sink->data_frames.fetch_add(1);
    } else {
        const uint8_t* input = kopaw::cpu_data(frame);
        if (!input || frame->format.audio.channels == 0 ||
            frame->size % (sizeof(float) * frame->format.audio.channels) != 0) {
            sink->bad_frame.store(true);
            frame->release(frame);
            return KOPAW_OK;
        }
        sink->sample_rate.store(frame->format.audio.sample_rate);
        sink->channels.store(frame->format.audio.channels);
        sink->samples.fetch_add(static_cast<uint32_t>(
            frame->size / (sizeof(float) * frame->format.audio.channels)));
        if (sink->data_frames.fetch_add(1) == 0 && frame->size >= sizeof(float)) {
            const auto* sample = reinterpret_cast<const float*>(input);
            sink->first_sample_milli.store(static_cast<int>(std::lround(sample[0] * 1000.0f)));
        }
    }
    frame->release(frame);
    return KOPAW_OK;
}

KopawNodeVTable source_vtable() {
    KopawNodeVTable vtable{};
    vtable.struct_size = sizeof(vtable);
    vtable.run = source_run;
    return vtable;
}

KopawNodeVTable sink_vtable() {
    KopawNodeVTable vtable{};
    vtable.struct_size = sizeof(vtable);
    vtable.send = sink_send;
    return vtable;
}

bool wait_finished(KopawGraph* graph) {
    for (int i = 0; i < 500; ++i) {
        if (kopaw_graph_state(graph) == KOPAW_STATE_FINISHED) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool run_video_test() {
    std::string error;
    auto* filter = new kopaw::VideoFilterNode();
    if (!filter->open("scale=2:1,format=gray", &error)) {
        fprintf(stderr, "video filter open failed: %s\n", error.c_str());
        delete filter;
        return false;
    }

    KopawGraph* graph = kopaw_graph_new();
    SourceState source{graph, {}, SourceKind::Video};
    SinkState sink{};
    sink.graph = graph;
    sink.expected_media = KOPAW_MEDIA_VIDEO;

    KopawNodeVTable source_vt = source_vtable();
    KopawNodeDesc source_desc{};
    source_desc.struct_size = sizeof(source_desc);
    source_desc.name = "test_video_source";
    source_desc.user_data = &source;
    source_desc.outputs = 1;
    source_desc.inputs = 0;
    source_desc.queue_capacity = 2;
    source_desc.vtable = &source_vt;
    const uint32_t source_id = kopaw_graph_add_node(graph, &source_desc);

    KopawNodeDesc filter_desc = filter->desc(graph);
    const uint32_t filter_id = kopaw_graph_add_node(graph, &filter_desc);
    filter->set_output(kopaw_graph_node_output(graph, filter_id, 0));

    KopawNodeVTable sink_vt = sink_vtable();
    KopawNodeDesc sink_desc{};
    sink_desc.struct_size = sizeof(sink_desc);
    sink_desc.name = "test_video_sink";
    sink_desc.user_data = &sink;
    sink_desc.outputs = 0;
    sink_desc.inputs = 1;
    sink_desc.queue_capacity = 4;
    sink_desc.is_sink = 1;
    sink_desc.vtable = &sink_vt;
    sink.node_id = kopaw_graph_add_node(graph, &sink_desc);
    source.output = kopaw_graph_node_output(graph, source_id, 0);

    const bool wired = source_id && filter_id && sink.node_id &&
                       kopaw_graph_connect(graph, source.output, filter_id, 0, 0) == KOPAW_OK &&
                       kopaw_graph_connect(graph, kopaw_graph_node_output(graph, filter_id, 0),
                                           sink.node_id, 0, 0) == KOPAW_OK;
    const bool started = wired && kopaw_graph_start(graph) == KOPAW_OK;
    const bool finished = started && wait_finished(graph);
    const bool passed = finished && !sink.bad_frame.load() && sink.data_frames.load() == 1 &&
                        sink.width.load() == 2 && sink.height.load() == 1 &&
                        sink.grayscale.load() && sink.first_pixel.load() != 0;
    if (!passed) {
        fprintf(stderr,
                "video lavfi test failed: wired=%d started=%d state=%d frames=%d %ux%u "
                "gray=%d pixel=0x%08x\n",
                wired, started, kopaw_graph_state(graph), sink.data_frames.load(),
                sink.width.load(), sink.height.load(), sink.grayscale.load(),
                sink.first_pixel.load());
    }
    kopaw_graph_free(graph);
    return passed;
}

bool run_audio_test() {
    std::string error;
    auto* filter = new kopaw::AudioFilterNode();
    if (!filter->open("volume=0.25,aresample=48000", &error)) {
        fprintf(stderr, "audio filter open failed: %s\n", error.c_str());
        delete filter;
        return false;
    }

    KopawGraph* graph = kopaw_graph_new();
    SourceState source{graph, {}, SourceKind::Audio};
    source.audio_rate = 44'100;
    source.audio_channels = 2;
    source.audio_samples = 441;
    source.audio_value = 1.0f;
    SinkState sink{};
    sink.graph = graph;
    sink.expected_media = KOPAW_MEDIA_AUDIO;

    KopawNodeVTable source_vt = source_vtable();
    KopawNodeDesc source_desc{};
    source_desc.struct_size = sizeof(source_desc);
    source_desc.name = "test_audio_source";
    source_desc.user_data = &source;
    source_desc.outputs = 1;
    source_desc.inputs = 0;
    source_desc.queue_capacity = 2;
    source_desc.vtable = &source_vt;
    const uint32_t source_id = kopaw_graph_add_node(graph, &source_desc);

    KopawNodeDesc filter_desc = filter->desc(graph);
    const uint32_t filter_id = kopaw_graph_add_node(graph, &filter_desc);
    filter->set_output(kopaw_graph_node_output(graph, filter_id, 0));

    KopawNodeVTable sink_vt = sink_vtable();
    KopawNodeDesc sink_desc{};
    sink_desc.struct_size = sizeof(sink_desc);
    sink_desc.name = "test_audio_sink";
    sink_desc.user_data = &sink;
    sink_desc.outputs = 0;
    sink_desc.inputs = 1;
    sink_desc.queue_capacity = 8;
    sink_desc.is_sink = 1;
    sink_desc.vtable = &sink_vt;
    sink.node_id = kopaw_graph_add_node(graph, &sink_desc);
    source.output = kopaw_graph_node_output(graph, source_id, 0);

    const bool wired = source_id && filter_id && sink.node_id &&
                       kopaw_graph_connect(graph, source.output, filter_id, 0, 0) == KOPAW_OK &&
                       kopaw_graph_connect(graph, kopaw_graph_node_output(graph, filter_id, 0),
                                           sink.node_id, 0, 0) == KOPAW_OK;
    const bool started = wired && kopaw_graph_start(graph) == KOPAW_OK;
    const bool finished = started && wait_finished(graph);
    const int sample = sink.first_sample_milli.load();
    const bool passed = finished && !sink.bad_frame.load() && sink.data_frames.load() > 0 &&
                        sink.sample_rate.load() == 48'000 && sink.channels.load() == 2 &&
                        sink.samples.load() >= 470 && sink.samples.load() <= 490 &&
                        std::abs(sample - 250) <= 25;
    if (!passed) {
        fprintf(stderr,
                "audio lavfi test failed: wired=%d started=%d state=%d frames=%d rate=%u ch=%u "
                "samples=%u first=%d\n",
                wired, started, kopaw_graph_state(graph), sink.data_frames.load(),
                sink.sample_rate.load(), sink.channels.load(), sink.samples.load(), sample);
    }
    kopaw_graph_free(graph);
    return passed;
}

bool run_invalid_chain_test() {
    std::string error;
    kopaw::VideoFilterNode video;
    const bool video_rejected = !video.open("filter_that_does_not_exist", &error) &&
                                !error.empty();
    error.clear();
    kopaw::AudioFilterNode audio;
    const bool audio_rejected = !audio.open("asplit=2", &error) && !error.empty();
    if (!video_rejected || !audio_rejected) {
        fprintf(stderr, "invalid lavfi chain was accepted: video=%d audio=%d\n",
                video_rejected, audio_rejected);
    }
    return video_rejected && audio_rejected;
}

bool run_invalid_frame_test() {
    std::string error;
    auto* filter = new kopaw::VideoFilterNode();
    if (!filter->open("null", &error)) {
        fprintf(stderr, "valid video filter open failed: %s\n", error.c_str());
        delete filter;
        return false;
    }
    KopawGraph* graph = kopaw_graph_new();
    filter->set_graph(graph);
    auto* frame = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 4);
    frame->frame.format.video.width = 2;
    frame->frame.format.video.height = 2;
    frame->frame.stride = 4;  // Less than the required 2 * 4 bytes per row.
    const int32_t rc = filter->send_impl(frame->ptr());
    const bool rejected = rc == KOPAW_E_INVALID;
    if (!rejected) fprintf(stderr, "invalid video frame was accepted: rc=%d\n", rc);
    // A non-OK send does not consume the frame under the KOPAW contract.
    frame->frame.release(frame->ptr());
    kopaw_graph_free(graph);
    delete filter;
    return rejected;
}

}  // namespace

int main() {
    return run_video_test() && run_audio_test() && run_invalid_chain_test() &&
                   run_invalid_frame_test()
               ? 0
               : 1;
}
