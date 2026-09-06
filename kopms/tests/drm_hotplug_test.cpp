#include <cstdio>
#include <string>

#include "drm_hotplug.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "drm-hotplug test: %s\n", message);
    return condition;
}

bool run() {
    if (!expect(std::string(kopms::drm_hotplug_action_name(
                             kopms::DrmHotplugAction::Added)) == "add",
                "added action name is wrong")) {
        return false;
    }
    if (!expect(std::string(kopms::drm_hotplug_action_name(
                             kopms::DrmHotplugAction::Removed)) == "remove",
                "removed action name is wrong")) {
        return false;
    }

    kopms::DrmHotplugMonitor monitor;
    if (!expect(monitor.fd() < 0 && !monitor.active(),
                "unstarted monitor exposes an active fd")) {
        return false;
    }
    std::string error;
    const bool started = monitor.start("/dev/dri/card0", {}, &error);
    if (started) {
        if (!expect(monitor.active() && monitor.fd() >= 0,
                    "started monitor has no readable fd")) {
            return false;
        }
        if (!expect(monitor.dispatch(&error), "empty udev dispatch failed")) return false;
    } else if (!expect(!error.empty(), "disabled monitor did not report a reason")) {
        return false;
    }
    monitor.stop();
    return expect(!monitor.active() && monitor.fd() < 0,
                  "stopped monitor remains active");
}

}  // namespace

int main() { return run() ? 0 : 1; }
