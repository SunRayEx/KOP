#include "drm_kms.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <poll.h>

#include <fcntl.h>
#include <unistd.h>

#ifdef KOPMS_HAVE_LIBDRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include "kop/log.h"

namespace kopms {

namespace {

constexpr uint32_t kPreferredModeFlag = 1u << 3;

bool connector_is_connected(uint32_t connection) {
#ifdef KOPMS_HAVE_LIBDRM
    return connection == DRM_MODE_CONNECTED;
#else
    return connection == 1;
#endif
}

bool connector_is_unknown(uint32_t connection) {
#ifdef KOPMS_HAVE_LIBDRM
    return connection == DRM_MODE_UNKNOWNCONNECTION;
#else
    return connection == 3;
#endif
}

const DrmKmsEncoder* find_encoder(const DrmKmsSnapshot& snapshot, uint32_t id) {
    for (const DrmKmsEncoder& encoder : snapshot.encoders) {
        if (encoder.id == id) return &encoder;
    }
    return nullptr;
}

const DrmKmsPlane* find_primary_plane(const DrmKmsSnapshot& snapshot,
                                      size_t crtc_index) {
    if (crtc_index >= std::numeric_limits<uint32_t>::digits) return nullptr;
    const uint32_t mask = UINT32_C(1) << crtc_index;
#ifdef KOPMS_HAVE_LIBDRM
    constexpr uint32_t primary_type = DRM_PLANE_TYPE_PRIMARY;
#else
    constexpr uint32_t primary_type = 1;
#endif
    for (const DrmKmsPlane& plane : snapshot.planes) {
        if (plane.type == primary_type && (plane.possible_crtcs & mask) != 0) {
            return &plane;
        }
    }
    return nullptr;
}

bool compatible_crtc(const DrmKmsEncoder& encoder, size_t crtc_index) {
    return crtc_index < std::numeric_limits<uint32_t>::digits &&
           (encoder.possible_crtcs & (UINT32_C(1) << crtc_index)) != 0;
}

bool choose_for_connector(const DrmKmsSnapshot& snapshot,
                          const DrmKmsConnector& connector,
                          DrmKmsSelection* selection) {
    if (!selection || connector.modes.empty()) return false;

    std::vector<uint32_t> encoder_ids;
    if (connector.encoder_id != 0) encoder_ids.push_back(connector.encoder_id);
    for (uint32_t id : connector.encoder_ids) {
        if (std::find(encoder_ids.begin(), encoder_ids.end(), id) == encoder_ids.end()) {
            encoder_ids.push_back(id);
        }
    }

    const auto preferred = std::find_if(
        connector.modes.begin(), connector.modes.end(),
        [](const DrmKmsMode& mode) { return mode.preferred(); });
    const DrmKmsMode& mode = preferred == connector.modes.end()
                                 ? connector.modes.front()
                                 : *preferred;

    for (uint32_t encoder_id : encoder_ids) {
        const DrmKmsEncoder* encoder = find_encoder(snapshot, encoder_id);
        if (!encoder) continue;
        for (size_t crtc_index = 0; crtc_index < snapshot.crtcs.size(); ++crtc_index) {
            if (!compatible_crtc(*encoder, crtc_index)) continue;
            const DrmKmsPlane* plane = find_primary_plane(snapshot, crtc_index);
            if (!plane) continue;

            selection->valid = true;
            selection->connector_id = connector.id;
            selection->encoder_id = encoder->id;
            selection->crtc_id = snapshot.crtcs[crtc_index].id;
            selection->primary_plane_id = plane->id;
            selection->crtc_index = crtc_index;
            selection->mode = mode;
            return true;
        }
    }
    return false;
}

#ifdef KOPMS_HAVE_LIBDRM

uint32_t plane_type(int fd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr properties =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties) return 0;

    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyPtr property = drmModeGetProperty(fd, properties->props[i]);
        if (!property) continue;
        if (std::strcmp(property->name, "type") == 0) {
            result = static_cast<uint32_t>(properties->prop_values[i]);
            drmModeFreeProperty(property);
            break;
        }
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

uint32_t property_id(int fd, uint32_t object_id, uint32_t object_type,
                     const char* name) {
    drmModeObjectPropertiesPtr properties =
        drmModeObjectGetProperties(fd, object_id, object_type);
    if (!properties) return 0;

    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyPtr property = drmModeGetProperty(fd, properties->props[i]);
        if (!property) continue;
        if (std::strcmp(property->name, name) == 0) {
            result = property->prop_id;
            drmModeFreeProperty(property);
            break;
        }
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

DrmKmsPropertySet read_atomic_properties(int fd, const DrmKmsSnapshot& snapshot) {
    DrmKmsPropertySet properties;
    if (!snapshot.selection.valid) return properties;
    properties.connector_crtc_id = property_id(
        fd, snapshot.selection.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    properties.crtc_active =
        property_id(fd, snapshot.selection.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    properties.crtc_mode_id =
        property_id(fd, snapshot.selection.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    properties.plane_fb_id = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    properties.plane_crtc_id = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    properties.plane_crtc_x = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    properties.plane_crtc_y = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    properties.plane_crtc_w = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    properties.plane_crtc_h = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    properties.plane_src_x = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    properties.plane_src_y = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    properties.plane_src_w = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    properties.plane_src_h = property_id(
        fd, snapshot.selection.primary_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    return properties;
}

void set_error(std::string* error, const char* prefix) {
    if (!error) return;
    *error = prefix;
    *error += std::strerror(errno);
}

bool discover_on_fd(int fd, const std::string& device, bool writable,
                    DrmKmsSnapshot* snapshot, std::string* error) {
    if (fd < 0 || !snapshot) {
        if (error) *error = "DRM/KMS 资源发现 fd 或快照目标无效";
        return false;
    }
    snapshot->device = device;

    if (!writable) {
        KOP_LOG_DEBUG("kopms-drm",
                      "DRM 设备以只读 fd 打开，跳过 client capability 设置");
    }

    drmVersionPtr version = drmGetVersion(fd);
    if (!version) {
        if (error) *error = "读取 DRM 版本失败";
        return false;
    }
    snapshot->driver_name = version->name ? version->name : "unknown";
    snapshot->driver_date = version->date ? version->date : "unknown";
    drmFreeVersion(version);

    snapshot->kms = drmIsKMS(fd) == 1;
    uint64_t addfb2_modifiers = 0;
    snapshot->addfb2_modifiers =
        drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &addfb2_modifiers) == 0 &&
        addfb2_modifiers != 0;
    if (writable) {
        snapshot->atomic = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
        snapshot->universal_planes = snapshot->atomic ||
                                     drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0;
    }

    drmModeResPtr resources = drmModeGetResources(fd);
    if (!resources) {
        set_error(error, "读取 DRM 资源失败: ");
        return false;
    }

    if (resources->count_crtcs > 0) {
        snapshot->crtcs.reserve(static_cast<size_t>(resources->count_crtcs));
    }
    for (int i = 0; i < resources->count_crtcs; ++i) {
        snapshot->crtcs.push_back({resources->crtcs[i]});
    }

    if (resources->count_connectors > 0) {
        snapshot->connectors.reserve(static_cast<size_t>(resources->count_connectors));
    }
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) continue;
        DrmKmsConnector copy;
        copy.id = connector->connector_id;
        copy.type = connector->connector_type;
        copy.type_id = connector->connector_type_id;
        copy.connection = static_cast<uint32_t>(connector->connection);
        copy.encoder_id = connector->encoder_id;
        if (const char* type_name = drmModeGetConnectorTypeName(copy.type)) {
            copy.type_name = type_name;
        }
        if (connector->count_encoders > 0) {
            copy.encoder_ids.assign(connector->encoders,
                                    connector->encoders + connector->count_encoders);
        }
        if (connector->count_modes > 0) {
            copy.modes.reserve(static_cast<size_t>(connector->count_modes));
        }
        for (int mode_index = 0; mode_index < connector->count_modes; ++mode_index) {
            const drmModeModeInfo& mode = connector->modes[mode_index];
            DrmKmsMode mode_copy;
            mode_copy.clock = mode.clock;
            mode_copy.width = mode.hdisplay;
            mode_copy.height = mode.vdisplay;
            mode_copy.refresh_hz = mode.vrefresh;
            mode_copy.flags = mode.flags;
            mode_copy.type = mode.type;
            mode_copy.hsync_start = mode.hsync_start;
            mode_copy.hsync_end = mode.hsync_end;
            mode_copy.htotal = mode.htotal;
            mode_copy.hskew = mode.hskew;
            mode_copy.vsync_start = mode.vsync_start;
            mode_copy.vsync_end = mode.vsync_end;
            mode_copy.vtotal = mode.vtotal;
            mode_copy.vscan = mode.vscan;
            mode_copy.name = mode.name;
            copy.modes.push_back(std::move(mode_copy));
        }
        snapshot->connectors.push_back(std::move(copy));
        drmModeFreeConnector(connector);
    }

    if (resources->count_encoders > 0) {
        snapshot->encoders.reserve(static_cast<size_t>(resources->count_encoders));
    }
    for (int i = 0; i < resources->count_encoders; ++i) {
        drmModeEncoderPtr encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder) continue;
        snapshot->encoders.push_back(
            {encoder->encoder_id, encoder->crtc_id, encoder->possible_crtcs});
        drmModeFreeEncoder(encoder);
    }
    drmModeFreeResources(resources);

    if (snapshot->universal_planes) {
        drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
        if (planes) {
            snapshot->planes.reserve(planes->count_planes);
            for (uint32_t i = 0; i < planes->count_planes; ++i) {
                drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
                if (!plane) continue;
                DrmKmsPlane copy;
                copy.id = plane->plane_id;
                copy.possible_crtcs = plane->possible_crtcs;
                copy.type = plane_type(fd, plane->plane_id);
                if (plane->count_formats > 0) {
                    copy.formats.assign(plane->formats,
                                        plane->formats + plane->count_formats);
                }
                snapshot->planes.push_back(std::move(copy));
                drmModeFreePlane(plane);
            }
            drmModeFreePlaneResources(planes);
        } else {
            snapshot->universal_planes = false;
        }
    }

    select_output(*snapshot, &snapshot->selection, &snapshot->selection_error);
    if (snapshot->selection.valid && snapshot->atomic) {
        snapshot->properties = read_atomic_properties(fd, *snapshot);
        if (!snapshot->properties.modeset_ready()) {
            snapshot->selection_error = "atomic modeset property 不完整";
        }
    }
    if (!snapshot->kms) {
        if (error) *error = "DRM 设备不支持 KMS";
        return false;
    }
    return true;
}

drmModeModeInfo make_mode_info(const DrmKmsMode& source) {
    drmModeModeInfo mode{};
    mode.clock = source.clock;
    mode.hdisplay = static_cast<uint16_t>(source.width);
    mode.hsync_start = static_cast<uint16_t>(source.hsync_start);
    mode.hsync_end = static_cast<uint16_t>(source.hsync_end);
    mode.htotal = static_cast<uint16_t>(source.htotal);
    mode.hskew = static_cast<uint16_t>(source.hskew);
    mode.vdisplay = static_cast<uint16_t>(source.height);
    mode.vsync_start = static_cast<uint16_t>(source.vsync_start);
    mode.vsync_end = static_cast<uint16_t>(source.vsync_end);
    mode.vtotal = static_cast<uint16_t>(source.vtotal);
    mode.vscan = static_cast<uint16_t>(source.vscan);
    mode.vrefresh = source.refresh_hz;
    mode.flags = source.flags;
    mode.type = source.type;
    std::snprintf(mode.name, sizeof(mode.name), "%s", source.name.c_str());
    return mode;
}

bool add_atomic_property(drmModeAtomicReqPtr request, uint32_t object_id,
                         uint32_t property, uint64_t value, std::string* error) {
    if (drmModeAtomicAddProperty(request, object_id, property, value) < 0) {
        set_error(error, "加入 atomic property 失败: ");
        return false;
    }
    return true;
}

bool commit_modeset(int fd, const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                    uint32_t framebuffer_width, uint32_t framebuffer_height,
                    uint32_t flags, std::string* error) {
    if (framebuffer_width == 0 || framebuffer_height == 0) {
        if (error) *error = "atomic framebuffer 尺寸无效";
        return false;
    }

    drmModeModeInfo mode = make_mode_info(snapshot.selection.mode);
    uint32_t mode_blob = 0;
    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob) < 0) {
        set_error(error, "创建 mode blob 失败: ");
        return false;
    }

    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) {
        if (error) *error = "分配 atomic request 失败";
        drmModeDestroyPropertyBlob(fd, mode_blob);
        return false;
    }

    const DrmKmsSelection& selection = snapshot.selection;
    const DrmKmsPropertySet& properties = snapshot.properties;
    bool valid = add_atomic_property(request, selection.connector_id,
                                     properties.connector_crtc_id, selection.crtc_id,
                                     error) &&
                 add_atomic_property(request, selection.crtc_id, properties.crtc_active, 1,
                                     error) &&
                 add_atomic_property(request, selection.crtc_id, properties.crtc_mode_id,
                                     mode_blob, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_fb_id, framebuffer_id, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_id, selection.crtc_id, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_x, 0, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_y, 0, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_w, selection.mode.width, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_h, selection.mode.height, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_src_x, 0, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_src_y, 0, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_src_w,
                                     static_cast<uint64_t>(framebuffer_width) << 16, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_src_h,
                                     static_cast<uint64_t>(framebuffer_height) << 16, error);
    int result = valid ? drmModeAtomicCommit(fd, request, flags, nullptr) : -1;
    drmModeAtomicFree(request);
    drmModeDestroyPropertyBlob(fd, mode_blob);
    if (!valid) return false;
    if (result < 0) {
        set_error(error, "atomic modeset 提交失败: ");
        return false;
    }
    return true;
}

bool commit_disable(int fd, const DrmKmsSnapshot& snapshot, std::string* error) {
    const DrmKmsSelection& selection = snapshot.selection;
    const DrmKmsPropertySet& properties = snapshot.properties;
    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) {
        if (error) *error = "分配 atomic disable request 失败";
        return false;
    }
    bool valid = add_atomic_property(request, selection.connector_id,
                                     properties.connector_crtc_id, 0, error) &&
                 add_atomic_property(request, selection.crtc_id, properties.crtc_active, 0,
                                     error) &&
                 add_atomic_property(request, selection.crtc_id, properties.crtc_mode_id, 0,
                                     error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_fb_id, 0, error) &&
                 add_atomic_property(request, selection.primary_plane_id,
                                     properties.plane_crtc_id, 0, error);
    const int result = valid ? drmModeAtomicCommit(fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET,
                                                    nullptr)
                             : -1;
    drmModeAtomicFree(request);
    if (!valid) return false;
    if (result < 0) {
        set_error(error, "atomic disable 提交失败: ");
        return false;
    }
    return true;
}

#endif

}  // namespace

bool DrmKmsMode::preferred() const noexcept { return (type & kPreferredModeFlag) != 0; }

bool select_output(const DrmKmsSnapshot& snapshot, DrmKmsSelection* selection,
                   std::string* error) {
    if (!selection) {
        if (error) *error = "DRM/KMS 输出选择目标为空";
        return false;
    }
    *selection = {};

    // Connected outputs win over unknown-connection outputs. Never return a
    // connector without a complete CRTC and primary-plane path.
    for (const DrmKmsConnector& connector : snapshot.connectors) {
        if (!connector_is_connected(connector.connection)) continue;
        if (choose_for_connector(snapshot, connector, selection)) return true;
    }
    for (const DrmKmsConnector& connector : snapshot.connectors) {
        if (!connector_is_unknown(connector.connection)) continue;
        if (choose_for_connector(snapshot, connector, selection)) return true;
    }
    if (error) {
        *error = snapshot.connectors.empty()
                     ? "DRM/KMS 没有 connector"
                     : "没有找到兼容的 connector/encoder/CRTC/primary plane";
    }
    return false;
}

bool DrmKmsPropertySet::modeset_ready() const noexcept {
    return connector_crtc_id != 0 && crtc_active != 0 && crtc_mode_id != 0 &&
           plane_fb_id != 0 && plane_crtc_id != 0 && plane_crtc_x != 0 &&
           plane_crtc_y != 0 && plane_crtc_w != 0 && plane_crtc_h != 0 &&
           plane_src_x != 0 && plane_src_y != 0 && plane_src_w != 0 &&
           plane_src_h != 0;
}

bool validate_atomic_output(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                            std::string* error) {
    if (!snapshot.atomic) {
        if (error) *error = "DRM 设备未协商 atomic capability";
        return false;
    }
    if (!snapshot.selection.valid) {
        if (error) *error = "没有可用于 atomic 输出的资源选择";
        return false;
    }
    if (framebuffer_id == 0) {
        if (error) *error = "atomic 输出 framebuffer id 无效";
        return false;
    }
    if (!snapshot.properties.modeset_ready()) {
        if (error) *error = "atomic 输出 property 不完整";
        return false;
    }
    const DrmKmsMode& mode = snapshot.selection.mode;
    if (mode.width == 0 || mode.height == 0 || mode.clock == 0 ||
        mode.htotal == 0 || mode.vtotal == 0 ||
        mode.width > std::numeric_limits<uint16_t>::max() ||
        mode.height > std::numeric_limits<uint16_t>::max() ||
        mode.hsync_start > std::numeric_limits<uint16_t>::max() ||
        mode.hsync_end > std::numeric_limits<uint16_t>::max() ||
        mode.htotal > std::numeric_limits<uint16_t>::max() ||
        mode.hskew > std::numeric_limits<uint16_t>::max() ||
        mode.vsync_start > std::numeric_limits<uint16_t>::max() ||
        mode.vsync_end > std::numeric_limits<uint16_t>::max() ||
        mode.vtotal > std::numeric_limits<uint16_t>::max() ||
        mode.vscan > std::numeric_limits<uint16_t>::max()) {
        if (error) *error = "atomic 输出 mode timing 不完整";
        return false;
    }
    return true;
}

bool DrmKmsGuard::discover(DrmKmsSnapshot* snapshot, std::string* error) const {
    if (!snapshot) {
        if (error) *error = "DRM/KMS 资源快照目标为空";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    *snapshot = {};
    snapshot->device = device_;
    if (error) *error = "libdrm 不可用，DRM/KMS 资源发现未启用";
    return false;
#else
    // Opening a primary node does not acquire DRM master. No modeset ioctl is
    // issued here; this function only reads resource metadata and client caps.
    bool writable = true;
    int fd = open(device_.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        const int open_error = errno;
        writable = false;
        fd = open(device_.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            errno = open_error;
            set_error(error, "打开 DRM 设备失败: ");
            return false;
        }
    }
    *snapshot = {};
    const bool result = discover_on_fd(fd, device_, writable, snapshot, error);
    close(fd);
    return result;
#endif
}

bool DrmKmsGuard::discover_fd(int fd, DrmKmsSnapshot* snapshot,
                              std::string* error) const {
    if (!snapshot) {
        if (error) *error = "DRM/KMS 资源快照目标为空";
        return false;
    }
    *snapshot = {};
    snapshot->device = device_;
#ifndef KOPMS_HAVE_LIBDRM
    (void)fd;
    if (error) *error = "libdrm 不可用，DRM/KMS 资源发现未启用";
    return false;
#else
    if (fd < 0) {
        if (error) *error = "DRM/KMS 资源发现 fd 无效";
        return false;
    }
    return discover_on_fd(fd, device_, true, snapshot, error);
#endif
}

bool DrmKmsGuard::probe(std::string* error) const {
    DrmKmsSnapshot snapshot;
    return discover(&snapshot, error);
}

DrmKmsSession::~DrmKmsSession() { close(); }

bool DrmKmsSession::open(const std::string& device, std::string* error) {
    close();
    if (device.empty()) {
        if (error) *error = "DRM 设备路径为空";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    (void)device;
    if (error) *error = "libdrm 不可用，DRM session 未启用";
    return false;
#else
    const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        set_error(error, "打开 DRM session 失败: ");
        return false;
    }
    return open_fd(fd, device, error);
#endif
}

bool DrmKmsSession::open_fd(int fd, const std::string& device, std::string* error) {
    close();
    if (fd < 0) {
        if (error) *error = "DRM session fd 无效";
        return false;
    }
    if (device.empty()) {
        ::close(fd);
        if (error) *error = "DRM 设备路径为空";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    ::close(fd);
    if (error) *error = "libdrm 不可用，DRM session 未启用";
    return false;
#else
    if (drmIsKMS(fd) != 1) {
        if (error) *error = "DRM 设备不支持 KMS";
        ::close(fd);
        return false;
    }
    device_ = device;
    fd_ = fd;
    atomic_enabled_ = drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
    return true;
#endif
}

bool DrmKmsSession::acquire_master(std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (master_) return true;
#ifndef KOPMS_HAVE_LIBDRM
    if (error) *error = "libdrm 不可用，DRM master 未启用";
    return false;
#else
    if (drmSetMaster(fd_) < 0) {
        set_error(error, "取得 DRM master 失败: ");
        return false;
    }
    master_ = true;
    return true;
#endif
}

bool DrmKmsSession::drop_master(std::string* error) {
    if (!master_) return true;
    if (page_flip_pending_) {
        if (error) *error = "page-flip 仍在等待，不能释放 DRM master";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    if (error) *error = "libdrm 不可用，无法释放 DRM master";
    return false;
#else
    if (drmDropMaster(fd_) < 0) {
        set_error(error, "释放 DRM master 失败: ");
        return false;
    }
    master_ = false;
    return true;
#endif
}

bool DrmKmsSession::test_modeset(const DrmKmsSnapshot& snapshot,
                                 uint32_t framebuffer_id, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (!master_) {
        if (error) *error = "DRM session 不是 master";
        return false;
    }
    if (!atomic_enabled_) {
        if (error) *error = "DRM session 未启用 atomic capability";
        return false;
    }
    if (!validate_atomic_output(snapshot, framebuffer_id, error)) return false;
#ifndef KOPMS_HAVE_LIBDRM
    (void)snapshot;
    (void)framebuffer_id;
    return false;
#else
    return commit_modeset(fd_, snapshot, framebuffer_id, snapshot.selection.mode.width,
                          snapshot.selection.mode.height,
                          DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                          error);
#endif
}

bool DrmKmsSession::modeset(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                            std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (!master_) {
        if (error) *error = "DRM session 不是 master";
        return false;
    }
    if (!atomic_enabled_) {
        if (error) *error = "DRM session 未启用 atomic capability";
        return false;
    }
    if (!validate_atomic_output(snapshot, framebuffer_id, error)) return false;
#ifndef KOPMS_HAVE_LIBDRM
    (void)snapshot;
    (void)framebuffer_id;
    return false;
#else
    return commit_modeset(fd_, snapshot, framebuffer_id, snapshot.selection.mode.width,
                          snapshot.selection.mode.height,
                          DRM_MODE_ATOMIC_ALLOW_MODESET, error);
#endif
}

bool DrmKmsSession::disable_output(const DrmKmsSnapshot& snapshot, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (!master_) {
        if (error) *error = "DRM session 不是 master";
        return false;
    }
    if (!atomic_enabled_) {
        if (error) *error = "DRM session 未启用 atomic capability";
        return false;
    }
    if (!snapshot.selection.valid || !snapshot.properties.modeset_ready()) {
        if (error) *error = "atomic disable 输出 property 不完整";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    (void)snapshot;
    return false;
#else
    return commit_disable(fd_, snapshot, error);
#endif
}

bool DrmKmsSession::page_flip(const DrmKmsSnapshot& snapshot, uint32_t framebuffer_id,
                              std::function<void()> completed, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (!master_) {
        if (error) *error = "DRM session 不是 master";
        return false;
    }
    if (!atomic_enabled_) {
        if (error) *error = "DRM session 未启用 atomic capability";
        return false;
    }
    if (page_flip_pending_) {
        if (error) *error = "已有 page-flip 在等待";
        return false;
    }
    if (!validate_atomic_output(snapshot, framebuffer_id, error)) return false;
#ifndef KOPMS_HAVE_LIBDRM
    (void)snapshot;
    (void)framebuffer_id;
    (void)completed;
    return false;
#else
    drmModeAtomicReqPtr request = drmModeAtomicAlloc();
    if (!request) {
        if (error) *error = "分配 page-flip atomic request 失败";
        return false;
    }
    if (!add_atomic_property(request, snapshot.selection.primary_plane_id,
                             snapshot.properties.plane_fb_id, framebuffer_id, error)) {
        drmModeAtomicFree(request);
        return false;
    }

    page_flip_handler_ = std::move(completed);
    page_flip_pending_ = true;
    const int result = drmModeAtomicCommit(
        fd_, request, DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, this);
    drmModeAtomicFree(request);
    if (result < 0) {
        page_flip_pending_ = false;
        page_flip_handler_ = {};
        set_error(error, "atomic page-flip 提交失败: ");
        return false;
    }
    return true;
#endif
}

bool DrmKmsSession::wait_for_page_flip(int timeout_ms, std::string* error) {
    if (fd_ < 0) {
        if (error) *error = "DRM session 尚未打开";
        return false;
    }
    if (!page_flip_pending_) {
        if (error) *error = "没有等待中的 page-flip";
        return false;
    }
#ifndef KOPMS_HAVE_LIBDRM
    (void)timeout_ms;
    if (error) *error = "libdrm 不可用，page-flip event 未启用";
    return false;
#else
    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
        if (error) *error = "page-flip event 等待超时";
        return false;
    }
    if (result < 0) {
        set_error(error, "等待 page-flip event 失败: ");
        return false;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        if (error) *error = "DRM page-flip event fd 状态无效";
        return false;
    }
    drmEventContext context{};
    context.version = DRM_EVENT_CONTEXT_VERSION;
    context.page_flip_handler = &DrmKmsSession::page_flip_event;
    if (drmHandleEvent(fd_, &context) < 0) {
        set_error(error, "处理 page-flip event 失败: ");
        return false;
    }
    if (page_flip_pending_) {
        if (error) *error = "DRM 未返回 page-flip event";
        return false;
    }
    return true;
#endif
}

void DrmKmsSession::page_flip_event(int fd, unsigned int frame, unsigned int seconds,
                                    unsigned int useconds, void* data) {
    (void)fd;
    (void)frame;
    (void)seconds;
    (void)useconds;
    auto* session = static_cast<DrmKmsSession*>(data);
    if (!session) return;
    session->page_flip_pending_ = false;
    std::function<void()> handler = std::move(session->page_flip_handler_);
    session->page_flip_handler_ = {};
    if (handler) handler();
}

void DrmKmsSession::close() {
    if (fd_ < 0) return;
#ifdef KOPMS_HAVE_LIBDRM
    if (master_) {
        std::string ignored;
        drop_master(&ignored);
    }
#endif
    ::close(fd_);
    fd_ = -1;
    master_ = false;
    atomic_enabled_ = false;
    page_flip_pending_ = false;
    page_flip_handler_ = {};
    device_.clear();
}

}  // namespace kopms
