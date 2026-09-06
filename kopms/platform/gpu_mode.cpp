#include "gpu_mode.hpp"

#include <cstdlib>
#include <cstring>

namespace kopms {

const char* gpu_mode_name(GpuMode mode) noexcept {
    switch (mode) {
        case GpuMode::Single:
            return "SINGLE";
        case GpuMode::Hybrid:
            return "HYBRID";
    }
    return "UNKNOWN";
}

bool parse_gpu_mode(const char* value, GpuMode* mode, std::string* error) {
    if (!value || !mode || value[0] == '\0') {
        if (error) *error = "KOPMS_GPU_MODE must be SINGLE or HYBRID";
        return false;
    }
    if (std::strcmp(value, "SINGLE") == 0) {
        *mode = GpuMode::Single;
        return true;
    }
    if (std::strcmp(value, "HYBRID") == 0) {
        *mode = GpuMode::Hybrid;
        return true;
    }
    if (error) {
        *error = "invalid KOPMS_GPU_MODE='" + std::string(value) +
                 "' (expected SINGLE or HYBRID)";
    }
    return false;
}

GpuMode gpu_mode_from_environment(std::string* diagnostic) {
    if (diagnostic) diagnostic->clear();
    const char* value = std::getenv(KOPMS_GPU_MODE_ENV);
    if (!value || value[0] == '\0') return GpuMode::Single;

    GpuMode mode = GpuMode::Single;
    std::string error;
    if (parse_gpu_mode(value, &mode, &error)) return mode;
    if (diagnostic) *diagnostic = error;
    return GpuMode::Single;
}

}  // namespace kopms
