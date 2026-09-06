#include "drm_hotplug.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>

#ifdef KOPMS_HAVE_LIBUDEV
#include <libudev.h>
#endif

namespace kopms {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

}  // namespace

const char* drm_hotplug_action_name(DrmHotplugAction action) noexcept {
    switch (action) {
        case DrmHotplugAction::Added:
            return "add";
        case DrmHotplugAction::Removed:
            return "remove";
        case DrmHotplugAction::Changed:
            return "change";
    }
    return "unknown";
}

struct DrmHotplugMonitor::Impl {
    std::string device;
    Callback callback;
#ifdef KOPMS_HAVE_LIBUDEV
    udev* context = nullptr;
    udev_monitor* monitor = nullptr;
#endif
};

DrmHotplugMonitor::DrmHotplugMonitor() : impl_(std::make_unique<Impl>()) {}

DrmHotplugMonitor::~DrmHotplugMonitor() { stop(); }

bool DrmHotplugMonitor::start(const std::string& drm_device, Callback callback,
                              std::string* error) {
    stop();
    if (drm_device.empty()) {
        set_error(error, "DRM hotplug device path is empty");
        return false;
    }
    impl_->device = drm_device;
    impl_->callback = std::move(callback);
#ifndef KOPMS_HAVE_LIBUDEV
    set_error(error, "libudev 不可用，DRM hotplug monitor 未启用");
    impl_->callback = {};
    return false;
#else
    impl_->context = udev_new();
    if (!impl_->context) {
        set_error(error, "创建 udev context 失败");
        return false;
    }
    impl_->monitor = udev_monitor_new_from_netlink(impl_->context, "udev");
    if (!impl_->monitor) {
        set_error(error, "创建 DRM udev monitor 失败");
        stop();
        return false;
    }
    if (udev_monitor_filter_add_match_subsystem_devtype(impl_->monitor, "drm", nullptr) < 0 ||
        udev_monitor_enable_receiving(impl_->monitor) < 0) {
        set_error(error, "启用 DRM udev monitor 失败");
        stop();
        return false;
    }
    const int monitor_fd = udev_monitor_get_fd(impl_->monitor);
    const int flags = monitor_fd >= 0 ? fcntl(monitor_fd, F_GETFL) : -1;
    if (flags < 0 || fcntl(monitor_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        set_error(error, "设置 DRM udev monitor 非阻塞失败");
        stop();
        return false;
    }
    return true;
#endif
}

bool DrmHotplugMonitor::dispatch(std::string* error) {
#ifndef KOPMS_HAVE_LIBUDEV
    set_error(error, "libudev 不可用，DRM hotplug monitor 未启用");
    return false;
#else
    if (!impl_->monitor) {
        set_error(error, "DRM hotplug monitor 尚未启动");
        return false;
    }
    for (;;) {
        udev_device* device = udev_monitor_receive_device(impl_->monitor);
        if (!device) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == 0) return true;
            set_error(error, std::string("读取 DRM udev event 失败: ") + std::strerror(errno));
            return false;
        }
        const char* action = udev_device_get_action(device);
        const char* devnode = udev_device_get_devnode(device);
        const char* sysname = udev_device_get_sysname(device);
        const std::string event_device = devnode ? devnode : (sysname ? sysname : "");
        const std::string base = impl_->device.substr(impl_->device.find_last_of('/') + 1);
        // DRM connector events use names such as card0-HDMI-A-1 rather than a
        // devnode. Match them to the selected card before notifying the output.
        if (!base.empty() && !event_device.empty() && event_device != impl_->device &&
            event_device != base && event_device.rfind(base + "-", 0) != 0) {
            udev_device_unref(device);
            continue;
        }
        DrmHotplugAction parsed;
        if (action && std::strcmp(action, "add") == 0) {
            parsed = DrmHotplugAction::Added;
        } else if (action && std::strcmp(action, "remove") == 0) {
            parsed = DrmHotplugAction::Removed;
        } else if (action && std::strcmp(action, "change") == 0) {
            parsed = DrmHotplugAction::Changed;
        } else {
            udev_device_unref(device);
            continue;
        }
        if (impl_->callback) impl_->callback(parsed, event_device);
        udev_device_unref(device);
    }
#endif
}

void DrmHotplugMonitor::stop() {
#ifdef KOPMS_HAVE_LIBUDEV
    if (impl_->monitor) impl_->monitor = udev_monitor_unref(impl_->monitor);
    if (impl_->context) impl_->context = udev_unref(impl_->context);
#endif
    impl_->device.clear();
    impl_->callback = {};
}

int DrmHotplugMonitor::fd() const noexcept {
#ifdef KOPMS_HAVE_LIBUDEV
    return impl_->monitor ? udev_monitor_get_fd(impl_->monitor) : -1;
#else
    return -1;
#endif
}

bool DrmHotplugMonitor::active() const noexcept {
#ifdef KOPMS_HAVE_LIBUDEV
    return impl_->monitor != nullptr;
#else
    return false;
#endif
}

}  // namespace kopms
