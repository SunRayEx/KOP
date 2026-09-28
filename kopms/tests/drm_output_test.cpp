#include <cstdio>
#include <string>

#include "drm_output.hpp"
#include "display_config.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "drm-output test: %s\n", message);
    return condition;
}

bool run() {
    kopms::DisplayConfig display;
    std::string error;
    if (!expect(kopms::parse_display_color_space("display-p3", &display.color_space),
                "display-p3 was not parsed")) return false;
    if (!expect(kopms::parse_hdr_mode("hdr10", &display.hdr),
                "hdr10 was not parsed")) return false;
    if (!expect(kopms::validate_display_config(display, &error),
                "valid display config was rejected")) return false;
    const char* spaces[] = {"srgb", "dci-p3", "wgc", "rec.709", "rec.2020",
                            "ntsc", "display-p3", "aces"};
    for (const char* name : spaces) {
        if (!expect(kopms::parse_display_color_space(name, &display.color_space),
                    "supported color space was not parsed")) return false;
    }

    kopms::DrmDirectOutput output;
    if (!expect(output.state() == kopms::DrmOutputState::Stopped,
                "new direct output is not stopped")) {
        return false;
    }
    if (!expect(!output.active(), "new direct output is active")) return false;

    if (!expect(!output.start(nullptr, "/dev/dri/card0", {}, &error),
                "null seat was accepted")) {
        return false;
    }
    if (!expect(!error.empty(), "invalid direct output did not report an error")) return false;

    output.handle_runtime_failure("test failure");
    if (!expect(output.state() == kopms::DrmOutputState::Revoked,
                "runtime failure did not revoke output")) {
        return false;
    }
    output.stop();
    return expect(output.state() == kopms::DrmOutputState::Stopped,
                  "stopped direct output has the wrong state");
}

}  // namespace

int main() { return run() ? 0 : 1; }
