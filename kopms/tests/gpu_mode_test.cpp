#include <cstdio>
#include <cstdlib>
#include <string>

#include "gpu_mode.hpp"

namespace {

bool check_parse(const char* text, kopms::GpuMode expected) {
    kopms::GpuMode mode = kopms::GpuMode::Hybrid;
    std::string error;
    if (!kopms::parse_gpu_mode(text, &mode, &error) || mode != expected) {
        std::fprintf(stderr, "failed to parse %s: %s\n", text ? text : "<null>",
                     error.c_str());
        return false;
    }
    return true;
}

bool run() {
    if (!check_parse("SINGLE", kopms::GpuMode::Single) ||
        !check_parse("HYBRID", kopms::GpuMode::Hybrid)) {
        return false;
    }

    kopms::GpuMode mode = kopms::GpuMode::Hybrid;
    std::string error;
    if (kopms::parse_gpu_mode("single", &mode, &error) ||
        kopms::parse_gpu_mode("GPU0", &mode, &error) || error.empty()) {
        std::fprintf(stderr, "invalid GPU mode was accepted\n");
        return false;
    }

    unsetenv(kopms::KOPMS_GPU_MODE_ENV);
    if (kopms::gpu_mode_from_environment() != kopms::GpuMode::Single) {
        return false;
    }
    setenv(kopms::KOPMS_GPU_MODE_ENV, "HYBRID", 1);
    if (kopms::gpu_mode_from_environment() != kopms::GpuMode::Hybrid) {
        return false;
    }
    std::string diagnostic;
    setenv(kopms::KOPMS_GPU_MODE_ENV, "invalid", 1);
    if (kopms::gpu_mode_from_environment(&diagnostic) != kopms::GpuMode::Single ||
        diagnostic.empty()) {
        return false;
    }
    unsetenv(kopms::KOPMS_GPU_MODE_ENV);
    return true;
}

}  // namespace

int main() { return run() ? 0 : 1; }
