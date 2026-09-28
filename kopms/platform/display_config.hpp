// KOPMS 输出显示配置：分辨率、缩放、色域和 HDR 能力的统一接口。
#pragma once

#include <cstdint>
#include <string>

namespace kopms {

enum class DisplayColorSpace : uint32_t {
    SRGB = 0,
    DCI_P3,
    WGC,
    REC709,
    REC2020,
    NTSC,
    DISPLAY_P3,
    ACES,
};

enum class HdrMode : uint32_t {
    Off = 0,
    Auto,
    HDR10,
    HLG,
};

struct DisplayConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    float scale = 1.0f;
    DisplayColorSpace color_space = DisplayColorSpace::SRGB;
    HdrMode hdr = HdrMode::Off;
    float sdr_white_nits = 203.0f;
    float peak_nits = 1000.0f;
};

const char* display_color_space_name(DisplayColorSpace value) noexcept;
const char* hdr_mode_name(HdrMode value) noexcept;
bool parse_display_color_space(const std::string& value, DisplayColorSpace* out);
bool parse_hdr_mode(const std::string& value, HdrMode* out);
bool validate_display_config(const DisplayConfig& config, std::string* error);

}  // namespace kopms
