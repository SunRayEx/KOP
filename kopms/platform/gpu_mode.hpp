// KOPMS GPU handle mode selected during process initialization.
#pragma once

#include <cstdint>
#include <string>

namespace kopms {

enum class GpuMode : uint32_t {
    Single = 0,
    Hybrid = 1,
};

constexpr const char* KOPMS_GPU_MODE_ENV = "KOPMS_GPU_MODE";

const char* gpu_mode_name(GpuMode mode) noexcept;

// Parse the documented values SINGLE and HYBRID. The caller retains ownership
// of the returned mode and can decide whether an invalid value is fatal.
bool parse_gpu_mode(const char* value, GpuMode* mode, std::string* error);

// Missing or invalid environment values select SINGLE. Invalid values are
// reported through diagnostic when it is non-null.
GpuMode gpu_mode_from_environment(std::string* diagnostic = nullptr);

inline bool gpu_mode_is_hybrid(GpuMode mode) noexcept {
    return mode == GpuMode::Hybrid;
}

}  // namespace kopms
