#include "drm_output.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#ifdef KOPMS_HAVE_LIBDRM
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
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

#ifdef KOPMS_HAVE_LIBDRM
struct DumbBuffer {
    int drm_fd = -1;
    uint32_t handle = 0;
    uint32_t framebuffer_id = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    void* mapping = MAP_FAILED;

    ~DumbBuffer() { reset(); }

    bool create(int fd, uint32_t width, uint32_t height, std::string* error) {
        reset();
        if (fd < 0 || width == 0 || height == 0) {
            set_error(error, "DRM dumb buffer 参数无效");
            return false;
        }
        drm_mode_create_dumb request{};
        request.width = width;
        request.height = height;
        request.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &request) < 0) {
            set_errno_error(error, "创建 DRM dumb buffer 失败: ");
            return false;
        }
        drm_fd = fd;
        handle = request.handle;
        pitch = request.pitch;
        size = request.size;

        uint32_t handles[4] = {handle, 0, 0, 0};
        uint32_t pitches[4] = {pitch, 0, 0, 0};
        uint32_t offsets[4] = {0, 0, 0, 0};
        if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches,
                          offsets, &framebuffer_id, 0) < 0) {
            set_errno_error(error, "创建 DRM dumb framebuffer 失败: ");
            reset();
            return false;
        }

        drm_mode_map_dumb map_request{};
        map_request.handle = handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) < 0) {
            set_errno_error(error, "映射 DRM dumb buffer 失败: ");
            reset();
            return false;
        }
        mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       static_cast<off_t>(map_request.offset));
        if (mapping == MAP_FAILED) {
            set_errno_error(error, "mmap DRM dumb buffer 失败: ");
            reset();
            return false;
        }
        // The bootstrap image is deliberately black. It is not a wl_shm or
        // KOPAW frame and exists only to validate the KMS transaction.
        std::memset(mapping, 0, static_cast<size_t>(size));
        return true;
    }

    void reset() noexcept {
        if (mapping != MAP_FAILED) {
            munmap(mapping, static_cast<size_t>(size));
            mapping = MAP_FAILED;
        }
        if (drm_fd >= 0 && framebuffer_id != 0) drmModeRmFB(drm_fd, framebuffer_id);
        if (drm_fd >= 0 && handle != 0) {
            drm_mode_destroy_dumb request{};
            request.handle = handle;
            ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &request);
        }
        drm_fd = -1;
        handle = 0;
        framebuffer_id = 0;
        pitch = 0;
        size = 0;
    }
};
#else
struct DumbBuffer {
    bool create(int, uint32_t, uint32_t, std::string* error) {
        set_error(error, "libdrm 不可用，物理输出未启用");
        return false;
    }
    void reset() noexcept {}
    uint32_t framebuffer_id = 0;
};
#endif

}  // namespace

const char* drm_output_state_name(DrmOutputState state) noexcept {
    switch (state) {
        case DrmOutputState::Stopped:
            return "stopped";
        case DrmOutputState::Starting:
            return "starting";
        case DrmOutputState::Active:
            return "active";
        case DrmOutputState::Paused:
            return "paused";
        case DrmOutputState::Revoked:
            return "revoked";
        case DrmOutputState::Failed:
            return "failed";
    }
    return "unknown";
}

struct DrmDirectOutput::Impl {
    SeatSession* seat = nullptr;
    std::string device;
    DrmOutputState state = DrmOutputState::Stopped;
    DrmDirectOutput::StateCallback callback;
    DrmKmsSession drm;
    DrmKmsSnapshot snapshot;
    std::unique_ptr<DumbBuffer> bootstrap_buffer;
    // M3：上一帧合成结果的 framebuffer（新 flip 提交前释放）
    DrmImportedBuffer presented_fb;

    void notify(DrmOutputState next, const std::string& reason) {
        state = next;
        if (callback) callback(state, reason);
    }

    void close_drm() {
        if (drm.is_master() && snapshot.selection.valid) {
            std::string error;
            if (!drm.disable_output(snapshot, &error)) {
                KOP_LOG_WARN("kopms-drm", "关闭 atomic 输出失败：%s", error.c_str());
            }
        }
        presented_fb.reset();
        bootstrap_buffer.reset();
        drm.close();
        snapshot = {};
    }
};

DrmDirectOutput::DrmDirectOutput() : impl_(std::make_unique<Impl>()) {}

DrmDirectOutput::~DrmDirectOutput() { stop(); }

bool activate_output(DrmDirectOutput::Impl* impl, bool existing_lease,
                     std::string* error) {
    if (!impl || !impl->seat || !impl->seat->active()) {
        set_error(error, "seat session 尚未授权 DRM 输出");
        return false;
    }

    int authorized_fd = -1;
    const bool acquired = existing_lease
                              ? impl->seat->duplicate_device(impl->device, &authorized_fd,
                                                             error)
                              : impl->seat->acquire_device(impl->device, &authorized_fd, error);
    if (!acquired) return false;
    if (!impl->drm.open_fd(authorized_fd, impl->device, error)) return false;

    DrmKmsGuard guard(impl->device);
    if (!guard.discover_fd(impl->drm.fd(), &impl->snapshot, error)) {
        impl->close_drm();
        return false;
    }
    if (!impl->snapshot.selection.valid) {
        set_error(error, impl->snapshot.selection_error.empty()
                            ? "DRM/KMS 没有完整输出候选"
                            : impl->snapshot.selection_error);
        impl->close_drm();
        return false;
    }
    if (!impl->snapshot.atomic || !impl->snapshot.properties.modeset_ready()) {
        set_error(error, "DRM/KMS atomic 输出能力或 property 不完整");
        impl->close_drm();
        return false;
    }

    impl->bootstrap_buffer = std::make_unique<DumbBuffer>();
    if (!impl->bootstrap_buffer->create(impl->drm.fd(), impl->snapshot.selection.mode.width,
                                        impl->snapshot.selection.mode.height, error)) {
        impl->close_drm();
        return false;
    }
    if (!impl->drm.acquire_master(error)) {
        impl->close_drm();
        return false;
    }
    if (!impl->drm.test_modeset(impl->snapshot, impl->bootstrap_buffer->framebuffer_id,
                                error)) {
        impl->close_drm();
        return false;
    }
    if (!impl->drm.modeset(impl->snapshot, impl->bootstrap_buffer->framebuffer_id, error)) {
        impl->close_drm();
        return false;
    }
    impl->notify(DrmOutputState::Active, "DRM atomic modeset active");
    return true;
}

bool DrmDirectOutput::start(SeatSession* seat, const std::string& device,
                            StateCallback callback, std::string* error) {
    stop();
    if (!seat || device.empty()) {
        set_error(error, "direct DRM output 参数无效");
        return false;
    }
    impl_->seat = seat;
    impl_->device = device;
    impl_->callback = std::move(callback);
    impl_->notify(DrmOutputState::Starting, "starting direct DRM output");
    if (!activate_output(impl_.get(), false, error)) {
        const std::string reason = error && !error->empty() ? *error : "DRM output activation failed";
        impl_->notify(DrmOutputState::Failed, reason);
        if (impl_->seat) impl_->seat->release_device(impl_->device, nullptr);
        impl_->close_drm();
        return false;
    }
    return true;
}

void DrmDirectOutput::stop() {
    if (!impl_) return;
    impl_->close_drm();
    if (impl_->seat && !impl_->device.empty()) {
        impl_->seat->release_device(impl_->device, nullptr);
    }
    impl_->seat = nullptr;
    impl_->device.clear();
    impl_->callback = {};
    impl_->state = DrmOutputState::Stopped;
}

bool DrmDirectOutput::handle_seat_state(SeatSessionState state,
                                        const std::string& reason,
                                        std::string* error) {
    if (!impl_->seat) {
        set_error(error, "direct DRM output 未绑定 seat session");
        return false;
    }
    if (state == SeatSessionState::Paused) {
        impl_->close_drm();
        impl_->notify(DrmOutputState::Paused, reason);
        return true;
    }
    if (state == SeatSessionState::Revoked) {
        impl_->close_drm();
        impl_->notify(DrmOutputState::Revoked, reason);
        return true;
    }
    if (state == SeatSessionState::Active && impl_->state != DrmOutputState::Active) {
        if (!activate_output(impl_.get(), true, error)) {
            const std::string failure = error && !error->empty() ? *error
                                                                   : "DRM resume failed";
            impl_->notify(DrmOutputState::Failed, failure);
            return false;
        }
        return true;
    }
    if (state == SeatSessionState::Failed) {
        impl_->close_drm();
        impl_->notify(DrmOutputState::Failed, reason);
        return false;
    }
    return true;
}

bool DrmDirectOutput::handle_hotplug(DrmHotplugAction action,
                                     const std::string& device,
                                     std::string* error) {
    const std::string base =
        impl_->device.substr(impl_->device.find_last_of('/') + 1);
    const bool card_event = device.empty() || device == impl_->device ||
                            device == base || device == base + "-" ||
                            device.rfind(base + "-", 0) == 0;
    if (card_event) {
        if (action == DrmHotplugAction::Removed) {
            impl_->close_drm();
            if (impl_->seat && (device.empty() || device == impl_->device || device == base)) {
                impl_->seat->release_device(impl_->device, nullptr);
            }
            impl_->notify(DrmOutputState::Revoked, "DRM device was removed");
            return true;
        }
        if (action == DrmHotplugAction::Changed && impl_->state == DrmOutputState::Active) {
            impl_->close_drm();
            if (!activate_output(impl_.get(), impl_->seat->has_device(impl_->device), error)) {
                impl_->notify(DrmOutputState::Revoked,
                              error && !error->empty() ? *error : "DRM output reconfiguration failed");
                return false;
            }
            return true;
        }
        if ((action == DrmHotplugAction::Added || action == DrmHotplugAction::Changed) &&
            impl_->state != DrmOutputState::Active && impl_->seat->active()) {
            if (!activate_output(impl_.get(), impl_->seat->has_device(impl_->device), error)) {
                impl_->notify(DrmOutputState::Failed,
                              error && !error->empty() ? *error : "DRM output recovery failed");
                return false;
            }
        }
    }
    return true;
}

void DrmDirectOutput::handle_runtime_failure(const std::string& reason) {
    impl_->close_drm();
    impl_->notify(DrmOutputState::Revoked,
                  reason.empty() ? "DRM runtime failure revoked output" : reason);
}

bool DrmDirectOutput::present_dmabuf(const DrmDmabufImportRequest& request,
                                     FlipDoneCallback completed, std::string* error) {
    if (!impl_ || !impl_->drm.is_master() || !impl_->snapshot.selection.valid) {
        set_error(error, "DRM 直出未激活，无法呈现合成结果");
        return false;
    }
    if (request.acquire_fence_kind == KOPAW_SYNC_FENCE_FD) {
        // 合成 fence 已在场景导出前等待；这里再守一道（M3 release 契约）。
        std::string fence_error;
        if (!wait_for_acquire_fence(request, 200, &fence_error)) {
            set_error(error, "合成结果 acquire fence 未完成：" + fence_error);
            return false;
        }
    }
    // 上一帧 FB 在新 flip 提交前释放：此刻显示的是它，AddFB2/flip 之后才
    // 离屏。page-flip 事件返回时 completed 触发，场景才释放源帧。
    DrmImportedBuffer framebuffer;
    if (!import_dmabuf_frame(impl_->drm.fd(), request, impl_->snapshot, &framebuffer,
                             0, error)) {
        return false;
    }
    if (!impl_->drm.page_flip(impl_->snapshot, framebuffer.framebuffer_id,
                              std::move(completed), error)) {
        return false;
    }
    if (!impl_->drm.wait_for_page_flip(200, error)) {
        // flip 未确认：保守保留 FB，等待恢复路径清理。
        KOP_LOG_WARN("kopms-drm", "page-flip 完成等待失败：%s", error->c_str());
        impl_->presented_fb = std::move(framebuffer);
        return false;
    }
    impl_->presented_fb = std::move(framebuffer);
    return true;
}

DrmOutputState DrmDirectOutput::state() const noexcept {
    return impl_ ? impl_->state : DrmOutputState::Stopped;
}

bool DrmDirectOutput::active() const noexcept {
    return state() == DrmOutputState::Active;
}

const DrmKmsSnapshot* DrmDirectOutput::snapshot() const noexcept {
    return impl_ && impl_->state == DrmOutputState::Active ? &impl_->snapshot : nullptr;
}

const std::string& DrmDirectOutput::device() const noexcept { return impl_->device; }

}  // namespace kopms
