#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "frame.hpp"
#include "plugin_loader.hpp"

namespace {

bool expect_reject(kopaw::PluginRegistry* registry,
                   const char* path,
                   const char* expected_fragment) {
    std::string error;
    if (registry->load(path, &error)) {
        fprintf(stderr, "unexpectedly loaded invalid plugin: %s\n", path);
        return false;
    }
    if (error.find(expected_fragment) == std::string::npos) {
        fprintf(stderr, "plugin rejection did not mention '%s': %s\n", expected_fragment,
                error.c_str());
        return false;
    }
    return true;
}

}  // namespace

namespace {

struct ForwardSource {
    KopawGraph* graph = nullptr;
    KopawOutput output{};
};

struct ForwardSink {
    KopawGraph* graph = nullptr;
    uint32_t node_id = 0;
    std::atomic<int> data_frames{0};
    std::atomic<int> eos_frames{0};
};

int32_t source_run(void* user) {
    auto* source = static_cast<ForwardSource*>(user);
    auto* data = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 4);
    if (kopaw_graph_emit(source->graph, source->output, data->ptr()) != KOPAW_OK) {
        return KOPAW_OK;
    }
    auto* eos = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 1, 1, 0);
    eos->frame.flags = KOPAW_FRAME_FLAG_EOS;
    kopaw_graph_emit(source->graph, source->output, eos->ptr());
    return KOPAW_OK;
}

int32_t sink_send(void* user, KopawFrame* frame) {
    auto* sink = static_cast<ForwardSink*>(user);
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        sink->eos_frames.fetch_add(1);
        frame->release(frame);
        kopaw_node_sink_done(sink->graph, sink->node_id);
    } else {
        sink->data_frames.fetch_add(1);
        frame->release(frame);
    }
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

bool run_forward_test(kopaw::PluginRegistry* registry,
                      const kopaw::LoadedPlugin* plugin,
                      uint32_t index) {
    std::string error;
    KopawGraph* graph = kopaw_graph_new();
    ForwardSource source{graph, {}};
    ForwardSink sink{graph};

    KopawNodeVTable source_vt = source_vtable();
    KopawNodeDesc source_desc{};
    source_desc.struct_size = sizeof(source_desc);
    source_desc.name = "fixture_source";
    source_desc.user_data = &source;
    source_desc.outputs = 1;
    source_desc.queue_capacity = 2;
    source_desc.vtable = &source_vt;
    const uint32_t source_id = kopaw_graph_add_node(graph, &source_desc);

    KopawNodeDesc plugin_desc{};
    if (!registry->create_node(plugin, index, graph, &plugin_desc, &error)) {
        fprintf(stderr, "forward fixture creation failed: %s\n", error.c_str());
        kopaw_graph_free(graph);
        return false;
    }
    const uint32_t plugin_id = kopaw_graph_add_node(graph, &plugin_desc);
    if (!plugin_id) {
        fprintf(stderr, "forward fixture plugin registration failed\n");
        if (plugin_desc.vtable && plugin_desc.vtable->destroy)
            plugin_desc.vtable->destroy(plugin_desc.user_data);
        kopaw_graph_free(graph);
        return false;
    }
    if (!registry->bind_node_outputs(&plugin_desc, graph, plugin_id, &error)) {
        fprintf(stderr, "forward fixture output binding failed: %s\n", error.c_str());
        kopaw_graph_free(graph);
        return false;
    }

    KopawNodeVTable sink_vt = sink_vtable();
    KopawNodeDesc sink_desc{};
    sink_desc.struct_size = sizeof(sink_desc);
    sink_desc.name = "fixture_sink";
    sink_desc.user_data = &sink;
    sink_desc.inputs = 1;
    sink_desc.queue_capacity = 2;
    sink_desc.is_sink = 1;
    sink_desc.vtable = &sink_vt;
    sink.node_id = kopaw_graph_add_node(graph, &sink_desc);

    source.output = kopaw_graph_node_output(graph, source_id, 0);
    const bool wired = source_id && plugin_id && sink.node_id && source.output.id != 0 &&
                       kopaw_graph_connect(graph, source.output, plugin_id, 0, 0) == KOPAW_OK &&
                       kopaw_graph_connect(graph, kopaw_graph_node_output(graph, plugin_id, 0),
                                           sink.node_id, 0, 0) == KOPAW_OK;
    const bool started = wired && kopaw_graph_start(graph) == KOPAW_OK;
    const bool finished = started && wait_finished(graph);
    const bool passed = finished && sink.data_frames.load() == 1 &&
                        sink.eos_frames.load() == 1;
    if (!passed) {
        fprintf(stderr,
                "plugin forwarding failed: wired=%d started=%d state=%d data=%d eos=%d\n",
                wired, started, kopaw_graph_state(graph), sink.data_frames.load(),
                sink.eos_frames.load());
    }
    kopaw_graph_free(graph);
    return passed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        fprintf(stderr, "expected six fixture plugin paths\n");
        return 2;
    }
    const char* good = argv[1];
    const char* bad_major = argv[2];
    const char* bad_size = argv[3];
    const char* bad_node = argv[4];
    const char* bad_binding = argv[5];
    const char* missing_symbol = argv[6];

    kopaw::PluginRegistry registry;
    if (!expect_reject(&registry, "/definitely/not/a/kopaw-plugin.so", "dlopen")) return 1;
    if (!expect_reject(&registry, missing_symbol, "kopaw_plugin_get_api")) return 1;
    if (!expect_reject(&registry, bad_major, "主版本")) return 1;
    if (!expect_reject(&registry, bad_size, "结构体过小")) return 1;
    if (!expect_reject(&registry, bad_node, "节点描述结构体过小")) return 1;
    if (!expect_reject(&registry, bad_binding, "bind_output")) return 1;

    const std::string marker = "/tmp/kopaw-plugin-unload-" + std::to_string(getpid());
    unlink(marker.c_str());
    setenv("KOPAW_TEST_UNLOAD_MARKER", marker.c_str(), 1);

    std::string error;
    if (!registry.load(good, &error)) {
        fprintf(stderr, "failed to load good plugin: %s\n", error.c_str());
        return 1;
    }
    uint32_t index = 0;
    const kopaw::LoadedPlugin* plugin = registry.find_node("fixture_passthrough", &index);
    if (!plugin || index != 0) {
        fprintf(stderr, "good plugin node was not registered\n");
        return 1;
    }

    KopawGraph* graph = kopaw_graph_new();
    KopawNodeDesc desc{};
    if (!registry.create_node(plugin, index, graph, &desc, &error)) {
        fprintf(stderr, "good plugin node creation failed: %s\n", error.c_str());
        kopaw_graph_free(graph);
        return 1;
    }
    if (!desc.vtable || !desc.vtable->destroy) {
        fprintf(stderr, "good plugin returned invalid node lifecycle\n");
        kopaw_graph_free(graph);
        return 1;
    }
    desc.vtable->destroy(desc.user_data);
    kopaw_graph_free(graph);

    if (!run_forward_test(&registry, plugin, index)) return 1;

    registry.unload_all();
    std::ifstream marker_file(marker);
    std::string contents;
    std::getline(marker_file, contents);
    unlink(marker.c_str());
    if (contents != "unloaded") {
        fprintf(stderr, "plugin destructor did not run on unload\n");
        return 1;
    }
    return 0;
}
