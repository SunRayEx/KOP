// P3-M2.4 optional DRM udev monitor.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace kopms {

enum class DrmHotplugAction {
    Added,
    Removed,
    Changed,
};

const char* drm_hotplug_action_name(DrmHotplugAction action) noexcept;

class DrmHotplugMonitor {
public:
    using Callback =
        std::function<void(DrmHotplugAction action, const std::string& device)>;

    DrmHotplugMonitor();
    ~DrmHotplugMonitor();

    DrmHotplugMonitor(const DrmHotplugMonitor&) = delete;
    DrmHotplugMonitor& operator=(const DrmHotplugMonitor&) = delete;

    bool start(const std::string& drm_device, Callback callback, std::string* error);
    bool dispatch(std::string* error);
    void stop();

    int fd() const noexcept;
    bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kopms
