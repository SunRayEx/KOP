#include "display_config.hpp"

#include <algorithm>
#include <cctype>

namespace kopms {
namespace {
std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
}  // namespace

const char* display_color_space_name(DisplayColorSpace value) noexcept {
    switch (value) {
        case DisplayColorSpace::SRGB: return "srgb";
        case DisplayColorSpace::DCI_P3: return "dci-p3";
        case DisplayColorSpace::WGC: return "wgc";
        case DisplayColorSpace::REC709: return "rec.709";
        case DisplayColorSpace::REC2020: return "rec.2020";
        case DisplayColorSpace::NTSC: return "ntsc";
        case DisplayColorSpace::DISPLAY_P3: return "display-p3";
        case DisplayColorSpace::ACES: return "aces";
    }
    return "unknown";
}

const char* hdr_mode_name(HdrMode value) noexcept {
    switch (value) {
        case HdrMode::Off: return "off";
        case HdrMode::Auto: return "auto";
        case HdrMode::HDR10: return "hdr10";
        case HdrMode::HLG: return "hlg";
    }
    return "unknown";
}

bool parse_display_color_space(const std::string& value, DisplayColorSpace* out) {
    if (!out) return false;
    const std::string v = lower(value);
    if (v == "srgb") *out = DisplayColorSpace::SRGB;
    else if (v == "dci-p3" || v == "dci_p3") *out = DisplayColorSpace::DCI_P3;
    else if (v == "wgc" || v == "wide-gamut") *out = DisplayColorSpace::WGC;
    else if (v == "rec.709" || v == "rec709" || v == "bt709") *out = DisplayColorSpace::REC709;
    else if (v == "rec.2020" || v == "rec2020" || v == "bt2020") *out = DisplayColorSpace::REC2020;
    else if (v == "ntsc") *out = DisplayColorSpace::NTSC;
    else if (v == "display-p3" || v == "display_p3") *out = DisplayColorSpace::DISPLAY_P3;
    else if (v == "aces" || v == "acescg") *out = DisplayColorSpace::ACES;
    else return false;
    return true;
}

bool parse_hdr_mode(const std::string& value, HdrMode* out) {
    if (!out) return false;
    const std::string v = lower(value);
    if (v == "off" || v == "sdr") *out = HdrMode::Off;
    else if (v == "auto") *out = HdrMode::Auto;
    else if (v == "hdr10" || v == "pq") *out = HdrMode::HDR10;
    else if (v == "hlg") *out = HdrMode::HLG;
    else return false;
    return true;
}

bool validate_display_config(const DisplayConfig& config, std::string* error) {
    if (config.width == 0 || config.height == 0) {
        if (error) *error = "display resolution must be non-zero";
        return false;
    }
    if (!(config.scale > 0.0f && config.scale <= 8.0f)) {
        if (error) *error = "display scale must be in (0, 8]";
        return false;
    }
    if (!(config.sdr_white_nits > 0.0f && config.peak_nits >= config.sdr_white_nits)) {
        if (error) *error = "invalid SDR white/peak luminance";
        return false;
    }
    return true;
}
}  // namespace kopms
