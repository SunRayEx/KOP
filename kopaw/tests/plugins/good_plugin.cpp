#include <cstdio>
#include <cstdlib>

#include "kopaw_plugin.h"

namespace {

struct Instance {
    KopawGraph* graph = nullptr;
    KopawOutput output{};
};

int32_t send(void* user, KopawFrame* frame) {
    auto* instance = static_cast<Instance*>(user);
    return kopaw_graph_emit(instance->graph, instance->output, frame);
}

void bind_output(void* user, uint32_t port, KopawOutput output) {
    auto* instance = static_cast<Instance*>(user);
    if (port == 0) instance->output = output;
}

void destroy(void* user) {
    delete static_cast<Instance*>(user);
}

const KopawNodeVTable kVTable = {
    sizeof(KopawNodeVTable),
    nullptr,
    send,
    nullptr,
    destroy,
    bind_output,
};

const KopawNodeDesc kDesc = {
    sizeof(KopawNodeDesc),
    "fixture_passthrough",
    nullptr,
    1,
    1,
    4,
    0,
    0,
    &kVTable,
};

const char* name() { return "fixture-good"; }
uint32_t count() { return 1; }
const KopawNodeDesc* desc(uint32_t index) { return index == 0 ? &kDesc : nullptr; }
int32_t create(uint32_t index, KopawGraph* graph, KopawNodeDesc* out) {
    if (index != 0 || !out) return KOPAW_E_INVALID;
    *out = kDesc;
    auto* instance = new Instance();
    instance->graph = graph;
    out->user_data = instance;
    return KOPAW_OK;
}

const KopawPlugin kApi = {
    sizeof(KopawPlugin),
    {sizeof(KopawAbiInfo), KOPAW_ABI_MAJOR, KOPAW_ABI_MINOR, KOPAW_ABI_CAPABILITIES},
    name,
    count,
    desc,
    create,
};

}  // namespace

extern "C" const KopawPlugin* kopaw_plugin_get_api() { return &kApi; }

__attribute__((destructor)) static void plugin_unloaded() {
    const char* marker = getenv("KOPAW_TEST_UNLOAD_MARKER");
    if (!marker || marker[0] == '\0') return;
    FILE* file = fopen(marker, "w");
    if (!file) return;
    fputs("unloaded\n", file);
    fclose(file);
}
