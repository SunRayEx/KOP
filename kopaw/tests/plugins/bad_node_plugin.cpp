#include "kopaw_plugin.h"

namespace {
const char* name() { return "fixture-bad-node"; }
uint32_t count() { return 1; }

const KopawNodeDesc kDesc = {
    0,
    "too_small",
    nullptr,
    0,
    1,
    1,
    0,
    0,
    nullptr,
};

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
