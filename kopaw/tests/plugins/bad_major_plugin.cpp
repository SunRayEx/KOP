#include "kopaw_plugin.h"

namespace {
const KopawPlugin kApi = {
    sizeof(KopawPlugin),
    {sizeof(KopawAbiInfo), KOPAW_ABI_MAJOR + 1, 0, 0},
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};
}  // namespace

extern "C" const KopawPlugin* kopaw_plugin_get_api() { return &kApi; }
