#include "kopaw_plugin.h"

namespace {
const KopawPlugin kApi = {
    0,
    {sizeof(KopawAbiInfo), KOPAW_ABI_MAJOR, KOPAW_ABI_MINOR, 0},
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};
}  // namespace

extern "C" const KopawPlugin* kopaw_plugin_get_api() { return &kApi; }
