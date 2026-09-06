#include "seat_session.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef KOPMS_HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

#include "kop/log.h"

namespace kopms {

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

void set_errno_error(std::string* error, const char* prefix) {
    if (!error) return;
    *error = prefix;
    *error += std::strerror(errno);
}

int duplicate_fd(int fd, int* copy, std::string* error) {
    if (!copy) {
        set_error(error, "seat device fd target is null");
        return -1;
    }
    *copy = -1;
    if (fd < 0) {
        set_error(error, "seat device fd is not available");
        return -1;
    }
    const int result = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (result < 0) {
        set_errno_error(error, "duplicate seat device fd failed: ");
        return -1;
    }
    *copy = result;
    return 0;
}

}  // namespace

const char* seat_session_state_name(SeatSessionState state) noexcept {
    switch (state) {
        case SeatSessionState::Stopped:
            return "stopped";
        case SeatSessionState::Active:
            return "active";
        case SeatSessionState::Paused:
            return "paused";
        case SeatSessionState::Revoked:
            return "revoked";
        case SeatSessionState::Failed:
            return "failed";
    }
    return "unknown";
}

const char* seat_session_backend_name(SeatSessionBackend backend) noexcept {
    switch (backend) {
        case SeatSessionBackend::None:
            return "none";
        case SeatSessionBackend::Logind:
            return "logind";
        case SeatSessionBackend::Unmanaged:
            return "unmanaged";
    }
    return "unknown";
}

struct SeatSession::Impl {
    struct DeviceLease {
        std::string path;
        unsigned major = 0;
        unsigned minor = 0;
        int fd = -1;
    };

    explicit Impl(std::string requested_seat, bool permit_unmanaged)
        : seat(std::move(requested_seat)), allow_unmanaged(permit_unmanaged) {}

    ~Impl() { close_devices(); }

    void close_devices() {
        for (auto& item : devices) {
            if (item.second.fd >= 0) ::close(item.second.fd);
            item.second.fd = -1;
        }
        devices.clear();
    }

    void notify(SeatSessionState next, const std::string& reason) {
        state = next;
        if (callback) callback(state, reason);
    }

    std::string seat;
    bool allow_unmanaged = false;
    SeatSessionState state = SeatSessionState::Stopped;
    SeatSessionBackend backend = SeatSessionBackend::None;
    std::string session_id;
    std::string session_path;
    SeatSession::StateCallback callback;
    std::map<std::string, DeviceLease> devices;

#ifdef KOPMS_HAVE_SYSTEMD
    sd_bus* bus = nullptr;
    sd_bus_slot* session_slot = nullptr;
#endif

    void close_bus() {
#ifdef KOPMS_HAVE_SYSTEMD
        session_slot = sd_bus_slot_unref(session_slot);
        bus = sd_bus_close_unref(bus);
#endif
    }
};

#ifdef KOPMS_HAVE_SYSTEMD
namespace {

constexpr const char* kLogindService = "org.freedesktop.login1";
constexpr const char* kLogindManagerPath = "/org/freedesktop/login1";
constexpr const char* kLogindManagerInterface = "org.freedesktop.login1.Manager";
constexpr const char* kLogindSessionInterface = "org.freedesktop.login1.Session";

std::string bus_error_text(const sd_bus_error& bus_error) {
    if (bus_error.message && bus_error.message[0] != '\0') {
        return bus_error.message;
    }
    if (bus_error.name && bus_error.name[0] != '\0') return bus_error.name;
    return "unknown systemd error";
}

bool check_active_logind_session(SeatSession::Impl* impl, std::string* error) {
    char* session = nullptr;
    const int session_result = sd_pid_get_session(getpid(), &session);
    if (session_result < 0 || !session || session[0] == '\0') {
        std::free(session);
        set_error(error, "当前进程没有 systemd-logind session");
        return false;
    }
    impl->session_id = session;
    std::free(session);

    char* session_seat = nullptr;
    const int seat_result = sd_session_get_seat(impl->session_id.c_str(), &session_seat);
    if (seat_result < 0 || !session_seat || session_seat[0] == '\0') {
        std::free(session_seat);
        set_error(error, "logind session 没有图形 seat");
        return false;
    }
    const bool seat_matches = impl->seat == session_seat;
    std::free(session_seat);
    if (!seat_matches) {
        set_error(error, "当前 logind session 不属于请求的 seat");
        return false;
    }
    if (sd_session_is_active(impl->session_id.c_str()) != 1) {
        set_error(error, "当前 logind session 不是 active session");
        return false;
    }
    return true;
}

bool get_logind_session_path(SeatSession::Impl* impl, std::string* error) {
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int result = sd_bus_call_method(
        impl->bus, kLogindService, kLogindManagerPath, kLogindManagerInterface,
        "GetSessionByPID", &bus_error, &reply, "u", static_cast<uint32_t>(getpid()));
    if (result < 0) {
        set_error(error, "GetSessionByPID 失败: " + bus_error_text(bus_error));
        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(reply);
        return false;
    }
    const char* path = nullptr;
    const int read_result = sd_bus_message_read(reply, "o", &path);
    if (read_result < 0 || !path || path[0] == '\0') {
        set_error(error, "GetSessionByPID 返回了无效 object path");
        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(reply);
        return false;
    }
    impl->session_path = path;
    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(reply);
    return true;
}

int session_signal(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto* impl = static_cast<SeatSession::Impl*>(userdata);
    if (!impl || !message) return 0;

    if (sd_bus_message_is_signal(message, kLogindSessionInterface, "PauseDevice")) {
        unsigned major = 0;
        unsigned minor = 0;
        const char* type = nullptr;
        if (sd_bus_message_read(message, "uus", &major, &minor, &type) < 0) return 0;
        for (auto& item : impl->devices) {
            auto& lease = item.second;
            if (lease.major != major || lease.minor != minor) continue;
            if (lease.fd >= 0) {
                ::close(lease.fd);
                lease.fd = -1;
            }
            const bool gone = type && std::strcmp(type, "gone") == 0;
            impl->notify(gone ? SeatSessionState::Revoked : SeatSessionState::Paused,
                         gone ? "logind revoked DRM device" : "logind paused DRM device");
            sd_bus_error bus_error = SD_BUS_ERROR_NULL;
            const int result = sd_bus_call_method(
                impl->bus, kLogindService, impl->session_path.c_str(),
                kLogindSessionInterface, "PauseDeviceComplete", &bus_error, nullptr,
                "uu", major, minor);
            if (result < 0) {
                KOP_LOG_WARN("kopms-seat", "PauseDeviceComplete 失败: %s",
                             bus_error_text(bus_error).c_str());
            }
            sd_bus_error_free(&bus_error);
            break;
        }
        return 0;
    }

    if (sd_bus_message_is_signal(message, kLogindSessionInterface, "ResumeDevice")) {
        unsigned major = 0;
        unsigned minor = 0;
        int fd = -1;
        if (sd_bus_message_read(message, "uuh", &major, &minor, &fd) < 0) return 0;
        for (auto& item : impl->devices) {
            auto& lease = item.second;
            if (lease.major != major || lease.minor != minor) continue;
            int copy = -1;
            std::string error;
            if (duplicate_fd(fd, &copy, &error) < 0) {
                impl->notify(SeatSessionState::Failed, error);
            } else {
                if (lease.fd >= 0) ::close(lease.fd);
                lease.fd = copy;
                impl->notify(SeatSessionState::Active, "logind resumed DRM device");
            }
            break;
        }
        return 0;
    }
    return 0;
}

bool start_logind(SeatSession::Impl* impl, std::string* error) {
    if (!check_active_logind_session(impl, error)) return false;
    int result = sd_bus_open_system(&impl->bus);
    if (result < 0 || !impl->bus) {
        set_error(error, "打开 system bus 失败");
        impl->close_bus();
        return false;
    }
    if (!get_logind_session_path(impl, error)) {
        impl->close_bus();
        return false;
    }
    const std::string match =
        "type='signal',sender='org.freedesktop.login1',path='" +
        impl->session_path + "',interface='org.freedesktop.login1.Session'";
    result = sd_bus_add_match(impl->bus, &impl->session_slot, match.c_str(), &session_signal,
                              impl);
    if (result < 0) {
        set_error(error, "订阅 logind session 信号失败");
        impl->close_bus();
        return false;
    }
    impl->backend = SeatSessionBackend::Logind;
    return true;
}

}  // namespace
#endif

SeatSession::SeatSession(std::string seat, bool allow_unmanaged)
    : impl_(std::make_unique<Impl>(std::move(seat), allow_unmanaged)) {}

SeatSession::~SeatSession() {
    stop();
}

bool SeatSession::start(StateCallback callback, std::string* error) {
    stop();
    impl_->callback = std::move(callback);
    if (impl_->seat.empty()) {
        impl_->notify(SeatSessionState::Failed, "seat 名称为空");
        set_error(error, "seat 名称为空");
        return false;
    }

#ifdef KOPMS_HAVE_SYSTEMD
    std::string logind_error;
    if (start_logind(impl_.get(), &logind_error)) {
        impl_->notify(SeatSessionState::Active, "systemd-logind session active");
        return true;
    }
    if (!impl_->allow_unmanaged) {
        impl_->notify(SeatSessionState::Failed, logind_error);
        set_error(error, logind_error);
        return false;
    }
    KOP_LOG_WARN("kopms-seat", "logind 不可用，使用显式 unmanaged DRM：%s",
                 logind_error.c_str());
#else
    if (!impl_->allow_unmanaged) {
        const std::string message = "未编译 systemd-logind，direct DRM 需要显式 unmanaged 许可";
        impl_->notify(SeatSessionState::Failed, message);
        set_error(error, message);
        return false;
    }
#endif

    impl_->backend = SeatSessionBackend::Unmanaged;
    impl_->notify(SeatSessionState::Active, "explicit unmanaged DRM session");
    return true;
}

bool SeatSession::dispatch(std::string* error) {
    if (impl_->state == SeatSessionState::Stopped ||
        impl_->state == SeatSessionState::Failed) {
        set_error(error, "seat session 尚未 active");
        return false;
    }
#ifdef KOPMS_HAVE_SYSTEMD
    if (impl_->backend == SeatSessionBackend::Logind && impl_->bus) {
        for (;;) {
            sd_bus_message* message = nullptr;
            const int result = sd_bus_process(impl_->bus, &message);
            sd_bus_message_unref(message);
            if (result < 0) {
                const std::string message_text = "处理 logind session 事件失败";
                impl_->notify(SeatSessionState::Revoked, message_text);
                set_error(error, message_text);
                return false;
            }
            if (result == 0) break;
        }
    }
#else
    (void)error;
#endif
    return true;
}

void SeatSession::stop() {
    if (!impl_) return;
    const bool already_stopped =
        impl_->state == SeatSessionState::Stopped &&
        impl_->backend == SeatSessionBackend::None && impl_->devices.empty() &&
        impl_->session_id.empty() && impl_->session_path.empty();
    if (already_stopped) return;
    std::vector<std::string> paths;
    paths.reserve(impl_->devices.size());
    for (const auto& item : impl_->devices) paths.push_back(item.first);
    for (const std::string& path : paths) release_device(path, nullptr);
    impl_->close_devices();
    impl_->close_bus();
    impl_->session_id.clear();
    impl_->session_path.clear();
    impl_->backend = SeatSessionBackend::None;
    impl_->notify(SeatSessionState::Stopped, "seat session stopped");
}

bool SeatSession::acquire_device(const std::string& path, int* fd, std::string* error) {
    if (!fd) {
        set_error(error, "seat device fd target is null");
        return false;
    }
    *fd = -1;
    if (!active()) {
        set_error(error, "seat session is not active");
        return false;
    }
    if (path.empty()) {
        set_error(error, "DRM device path is empty");
        return false;
    }
    const auto existing = impl_->devices.find(path);
    if (existing != impl_->devices.end()) {
        if (existing->second.fd >= 0) return duplicate_device(path, fd, error);
        // A logind "gone" event invalidates the old lease. Remove the stale
        // entry so a later udev add can request a fresh TakeDevice lease.
        impl_->devices.erase(existing);
    }

    struct stat device_stat {};
    if (stat(path.c_str(), &device_stat) < 0) {
        set_errno_error(error, "stat DRM device failed: ");
        return false;
    }
    if (!S_ISCHR(device_stat.st_mode)) {
        set_error(error, "DRM device is not a character device");
        return false;
    }

    Impl::DeviceLease lease;
    lease.path = path;
    lease.major = major(device_stat.st_rdev);
    lease.minor = minor(device_stat.st_rdev);

#ifdef KOPMS_HAVE_SYSTEMD
    if (impl_->backend == SeatSessionBackend::Logind) {
        sd_bus_error bus_error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int result = sd_bus_call_method(
            impl_->bus, "org.freedesktop.login1", impl_->session_path.c_str(),
            "org.freedesktop.login1.Session", "TakeDevice", &bus_error, &reply,
            "uu", lease.major, lease.minor);
        if (result < 0) {
            set_error(error, "logind TakeDevice 失败: " + bus_error_text(bus_error));
            sd_bus_error_free(&bus_error);
            sd_bus_message_unref(reply);
            return false;
        }
        int returned_fd = -1;
        int inactive = 0;
        if (sd_bus_message_read(reply, "hb", &returned_fd, &inactive) < 0 ||
            inactive != 0) {
            set_error(error, inactive != 0 ? "logind DRM device 当前 inactive"
                                           : "logind TakeDevice 返回无效 fd");
            sd_bus_error_free(&bus_error);
            sd_bus_message_unref(reply);
            return false;
        }
        if (duplicate_fd(returned_fd, &lease.fd, error) < 0) {
            sd_bus_error_free(&bus_error);
            sd_bus_message_unref(reply);
            return false;
        }
        sd_bus_error_free(&bus_error);
        sd_bus_message_unref(reply);
    } else
#endif
    {
        lease.fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (lease.fd < 0) {
            set_errno_error(error, "open unmanaged DRM device failed: ");
            return false;
        }
    }

    impl_->devices.emplace(path, std::move(lease));
    return duplicate_device(path, fd, error);
}

bool SeatSession::duplicate_device(const std::string& path, int* fd,
                                   std::string* error) const {
    if (!fd) {
        set_error(error, "seat device fd target is null");
        return false;
    }
    *fd = -1;
    const auto it = impl_->devices.find(path);
    if (it == impl_->devices.end() || it->second.fd < 0) {
        set_error(error, "seat device lease is not active");
        return false;
    }
    return duplicate_fd(it->second.fd, fd, error) == 0;
}

bool SeatSession::release_device(const std::string& path, std::string* error) {
    const auto it = impl_->devices.find(path);
    if (it == impl_->devices.end()) return true;
    const unsigned device_major = it->second.major;
    const unsigned device_minor = it->second.minor;
#ifdef KOPMS_HAVE_SYSTEMD
    if (impl_->backend == SeatSessionBackend::Logind && impl_->bus) {
        sd_bus_error bus_error = SD_BUS_ERROR_NULL;
        const int result = sd_bus_call_method(
            impl_->bus, "org.freedesktop.login1", impl_->session_path.c_str(),
            "org.freedesktop.login1.Session", "ReleaseDevice", &bus_error, nullptr,
            "uu", device_major, device_minor);
        if (result < 0) {
            set_error(error, "logind ReleaseDevice 失败: " + bus_error_text(bus_error));
            sd_bus_error_free(&bus_error);
            if (it->second.fd >= 0) ::close(it->second.fd);
            impl_->devices.erase(it);
            return false;
        }
        sd_bus_error_free(&bus_error);
    }
#else
    (void)device_major;
    (void)device_minor;
#endif
    if (it->second.fd >= 0) ::close(it->second.fd);
    impl_->devices.erase(it);
    return true;
}

bool SeatSession::active() const noexcept {
    return impl_ && impl_->state == SeatSessionState::Active;
}

bool SeatSession::has_device(const std::string& path) const noexcept {
    if (!impl_) return false;
    const auto it = impl_->devices.find(path);
    return it != impl_->devices.end() && it->second.fd >= 0;
}

SeatSessionState SeatSession::state() const noexcept {
    return impl_ ? impl_->state : SeatSessionState::Stopped;
}

SeatSessionBackend SeatSession::backend() const noexcept {
    return impl_ ? impl_->backend : SeatSessionBackend::None;
}

const std::string& SeatSession::seat() const noexcept { return impl_->seat; }

const std::string& SeatSession::session_id() const noexcept {
    return impl_->session_id;
}

int SeatSession::event_fd() const noexcept {
#ifdef KOPMS_HAVE_SYSTEMD
    if (impl_ && impl_->bus) return sd_bus_get_fd(impl_->bus);
#endif
    return -1;
}

}  // namespace kopms
