// P3-M2.4 seat/session ownership boundary.
//
// A DRM node is usable by KOPMS only while this object reports an active
// session. The logind backend owns the TakeDevice lease; the unmanaged
// backend is opt-in and intended for kiosk or test environments.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace kopms {

enum class SeatSessionState {
    Stopped,
    Active,
    Paused,
    Revoked,
    Failed,
};

enum class SeatSessionBackend {
    None,
    Logind,
    Unmanaged,
};

const char* seat_session_state_name(SeatSessionState state) noexcept;
const char* seat_session_backend_name(SeatSessionBackend backend) noexcept;

class SeatSession {
public:
    struct Impl;

    using StateCallback =
        std::function<void(SeatSessionState state, const std::string& reason)>;

    explicit SeatSession(std::string seat = "seat0", bool allow_unmanaged = false);
    ~SeatSession();

    SeatSession(const SeatSession&) = delete;
    SeatSession& operator=(const SeatSession&) = delete;

    // Start the session manager binding. This does not acquire a DRM node.
    bool start(StateCallback callback, std::string* error);
    // Drain logind events. It is safe to call this from a wl_event_loop tick.
    bool dispatch(std::string* error);
    void stop();

    // Acquire a device through the active session and return a new owned fd.
    // The session keeps its own lease fd so it can invalidate it on PauseDevice.
    bool acquire_device(const std::string& path, int* fd, std::string* error);
    bool duplicate_device(const std::string& path, int* fd, std::string* error) const;
    bool release_device(const std::string& path, std::string* error);

    bool active() const noexcept;
    bool has_device(const std::string& path) const noexcept;
    SeatSessionState state() const noexcept;
    SeatSessionBackend backend() const noexcept;
    const std::string& seat() const noexcept;
    const std::string& session_id() const noexcept;
    int event_fd() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace kopms
