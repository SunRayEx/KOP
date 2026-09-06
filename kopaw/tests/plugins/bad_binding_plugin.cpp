#include "kopaw_plugin.h"

namespace {

struct Instance {};

int32_t send(void* user, KopawFrame* frame) {
    (void)user;
    frame->release(frame);
    return KOPAW_OK;
}

void destroy(void* user) {
    delete static_cast<Instance*>(user);
}

// Deliberately omits bind_output while advertising one output port.
const KopawNodeVTable kVTable = {
    sizeof(KopawNodeVTable),
    nullptr,
    send,
    nullptr,
    destroy,
    nullptr,
};

const KopawNodeDesc kDesc = {
    sizeof(KopawNodeDesc),
    "missing_output_binding",
    nullptr,
    1,
    1,
    1,
    0,
    0,
    &kVTable,
};

const char* name() { return "fixture-bad-binding"; }
uint32_t count() { return 1; }
const KopawNodeDesc* desc(uint32_t index) { return index == 0 ? &kDesc : nullptr; }
int32_t create(uint32_t, KopawGraph*, KopawNodeDesc*) { return KOPAW_E_GENERIC; }

const KopawPlugin kApi = {
    sizeof(KopawPlugin),
    {sizeof(KopawAbiInfo), KOPAW_ABI_MAJOR, KOPAW_ABI_MINOR, 0},
    name,
    count,
    desc,
    create,
};

}  // namespace

extern "C" const KopawPlugin* kopaw_plugin_get_api() { return &kApi; }
