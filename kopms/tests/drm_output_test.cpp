#include <cstdio>
#include <string>

#include "drm_output.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "drm-output test: %s\n", message);
    return condition;
}

bool run() {
    kopms::DrmDirectOutput output;
    if (!expect(output.state() == kopms::DrmOutputState::Stopped,
                "new direct output is not stopped")) {
        return false;
    }
    if (!expect(!output.active(), "new direct output is active")) return false;

    std::string error;
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
