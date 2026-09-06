// P3-M2 DRM/KMS 资源发现边界。
//
// 这个头文件只暴露复制后的值类型，不把 libdrm 的资源指针或 DRM fd
// 泄漏到上层。discover() 仍然是只读探测：不会取得 master、修改 connector
// 或提交 modeset。真正的输出后端会在这个边界之后接入。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace kopms {

struct DrmKmsMode {
    uint32_t clock = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refresh_hz = 0;
    uint32_t flags = 0;
    uint32_t type = 0;
    uint32_t hsync_start = 0;
    uint32_t hsync_end = 0;
    uint32_t htotal = 0;
    uint32_t hskew = 0;
    uint32_t vsync_start = 0;
    uint32_t vsync_end = 0;
    uint32_t vtotal = 0;
    uint32_t vscan = 0;
    std::string name;

    bool preferred() const noexcept;
};

struct DrmKmsConnector {
    uint32_t id = 0;
    uint32_t type = 0;
    uint32_t type_id = 0;
    uint32_t connection = 0;
    uint32_t encoder_id = 0;
    std::string type_name;
    std::vector<uint32_t> encoder_ids;
    std::vector<DrmKmsMode> modes;
};

struct DrmKmsEncoder {
    uint32_t id = 0;
    uint32_t crtc_id = 0;
    uint32_t possible_crtcs = 0;
};

struct DrmKmsCrtc {
    uint32_t id = 0;
};

struct DrmKmsPlane {
    uint32_t id = 0;
    uint32_t possible_crtcs = 0;
    uint32_t type = 0;
    std::vector<uint32_t> formats;
};

struct DrmKmsSelection {
    bool valid = false;
    uint32_t connector_id = 0;
    uint32_t encoder_id = 0;
    uint32_t crtc_id = 0;
    uint32_t primary_plane_id = 0;
    size_t crtc_index = 0;
    DrmKmsMode mode;
};

struct DrmKmsPropertySet {
    uint32_t connector_crtc_id = 0;
    uint32_t crtc_active = 0;
    uint32_t crtc_mode_id = 0;
    uint32_t plane_fb_id = 0;
    uint32_t plane_crtc_id = 0;
    uint32_t plane_crtc_x = 0;
    uint32_t plane_crtc_y = 0;
    uint32_t plane_crtc_w = 0;
    uint32_t plane_crtc_h = 0;
    uint32_t plane_src_x = 0;
    uint32_t plane_src_y = 0;
    uint32_t plane_src_w = 0;
    uint32_t plane_src_h = 0;

    bool modeset_ready() const noexcept;
};

struct DrmKmsSnapshot {
    std::string device;
    std::string driver_name;
    std::string driver_date;
    bool kms = false;
    bool universal_planes = false;
    bool atomic = false;
    bool addfb2_modifiers = false;
    std::vector<DrmKmsConnector> connectors;
    std::vector<DrmKmsEncoder> encoders;
    std::vector<DrmKmsCrtc> crtcs;
    std::vector<DrmKmsPlane> planes;
    DrmKmsSelection selection;
    DrmKmsPropertySet properties;
    std::string selection_error;
};

// Select one connected output and a compatible encoder/CRTC/primary plane.
// Connected connectors win over unknown-connection connectors. This function
// is hardware-independent and is therefore also used by unit tests.
bool select_output(const DrmKmsSnapshot& snapshot, DrmKmsSelection* selection,
                   std::string* error);

// Validate the immutable prerequisites for an atomic modeset. The function
// does not access a DRM fd and is safe to run in CI without a GPU.
bool validate_atomic_output(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                            std::string* error);

class DrmKmsGuard {
public:
    explicit DrmKmsGuard(std::string device = "/dev/dri/card0")
        : device_(std::move(device)) {}

    bool discover(DrmKmsSnapshot* snapshot, std::string* error) const;
    // Discover resources on an fd that the caller already owns. This keeps
    // seat/logind authorization and the probed file description identical.
    bool discover_fd(int fd, DrmKmsSnapshot* snapshot, std::string* error) const;
    bool probe(std::string* error) const;
    const std::string& device() const { return device_; }

private:
    std::string device_;
};

// An explicit DRM file-description lifecycle. Construction/opening does not
// become DRM master; callers must acquire master only after their seat/session
// manager has granted the compositor access to the device.
class DrmKmsSession {
public:
    DrmKmsSession() = default;
    ~DrmKmsSession();

    DrmKmsSession(const DrmKmsSession&) = delete;
    DrmKmsSession& operator=(const DrmKmsSession&) = delete;

    bool open(const std::string& device, std::string* error);
    // Take ownership of an already-authorized DRM fd, for example one
    // returned by systemd-logind TakeDevice().
    bool open_fd(int fd, const std::string& device, std::string* error);
    bool acquire_master(std::string* error);
    bool drop_master(std::string* error = nullptr);
    bool test_modeset(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                      std::string* error);
    bool modeset(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                 std::string* error);
    bool disable_output(const DrmKmsSnapshot& snapshot, std::string* error);
    bool page_flip(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                   std::function<void()> completed, std::string* error);
    bool wait_for_page_flip(int timeout_ms, std::string* error);
    void close();

    int fd() const noexcept { return fd_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    bool is_master() const noexcept { return master_; }
    bool atomic_enabled() const noexcept { return atomic_enabled_; }
    const std::string& device() const noexcept { return device_; }

private:
    static void page_flip_event(int fd, unsigned int frame, unsigned int seconds,
                                unsigned int useconds, void* data);

    std::string device_;
    int fd_ = -1;
    bool master_ = false;
    bool atomic_enabled_ = false;
    bool page_flip_pending_ = false;
    std::function<void()> page_flip_handler_;
};

}  // namespace kopms
