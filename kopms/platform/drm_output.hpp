// P3-M2.4 DRM/KMS physical-output bootstrap.
//
// This controller owns only the KMS bootstrap buffer. KOPAW DMA-BUF frame
// submission is intentionally still a separate P3-M3 frame-bridge path.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "drm_buffer.hpp"
#include "drm_hotplug.hpp"
#include "drm_kms.hpp"
#include "seat_session.hpp"

namespace kopms {

enum class DrmOutputState {
    Stopped,
    Starting,
    Active,
    Paused,
    Revoked,
    Failed,
};

const char* drm_output_state_name(DrmOutputState state) noexcept;

class DrmDirectOutput {
public:
    struct Impl;

    using StateCallback =
        std::function<void(DrmOutputState state, const std::string& reason)>;
    // M3：合成结果 page-flip 完成回调（此刻才允许释放参与合成的场景帧）。
    using FlipDoneCallback = std::function<void()>;

    DrmDirectOutput();
    ~DrmDirectOutput();

    DrmDirectOutput(const DrmDirectOutput&) = delete;
    DrmDirectOutput& operator=(const DrmDirectOutput&) = delete;

    // Start physical output after the seat has authorized a DRM device.
    bool start(SeatSession* seat, const std::string& device, StateCallback callback,
               std::string* error);
    void stop();

    // Feed seat-manager and udev transitions to the output state machine.
    bool handle_seat_state(SeatSessionState state, const std::string& reason,
                           std::string* error);
    bool handle_hotplug(DrmHotplugAction action, const std::string& device,
                        std::string* error);
    void handle_runtime_failure(const std::string& reason);

    // M3 零拷贝直出：导入合成结果 DMA-BUF → AddFB2 → atomic page-flip，
    // 并同步等待 flip 完成事件后调用 completed。上一帧 FB 在新 flip 前释放。
    // request 的平面/fence fd 由调用方持有，本调用不关闭。
    bool present_dmabuf(const DrmDmabufImportRequest& request,
                        FlipDoneCallback completed, std::string* error);

    DrmOutputState state() const noexcept;
    bool active() const noexcept;
    const DrmKmsSnapshot* snapshot() const noexcept;
    const std::string& device() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace kopms
