#include <cstdio>
#include <string>

#include "drm_kms.hpp"

namespace {

kopms::DrmKmsMode mode(const char* name, uint32_t width, uint32_t height,
                       uint32_t type) {
    kopms::DrmKmsMode result;
    result.name = name;
    result.width = width;
    result.height = height;
    result.refresh_hz = 60;
    result.type = type;
    return result;
}

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "drm-kms test: %s\n", message);
    return condition;
}

bool run_selection_tests() {
    constexpr uint32_t kConnected = 1;
    constexpr uint32_t kUnknown = 3;
    constexpr uint32_t kPrimary = 1;
    constexpr uint32_t kPreferred = 1u << 3;

    kopms::DrmKmsSnapshot snapshot;
    snapshot.crtcs = {{100}, {101}};
    snapshot.encoders = {{200, 0, 1u << 1}, {201, 0, 1u << 0}};
    snapshot.planes = {{300, 1u << 0, kPrimary, {0x34325258}},
                       {301, 1u << 1, kPrimary, {0x34325258}}};

    kopms::DrmKmsConnector disconnected;
    disconnected.id = 10;
    disconnected.connection = 2;
    disconnected.encoder_id = 201;
    disconnected.modes.push_back(mode("1024x768", 1024, 768, kPreferred));
    snapshot.connectors.push_back(disconnected);

    kopms::DrmKmsConnector connected;
    connected.id = 11;
    connected.connection = kConnected;
    connected.encoder_id = 200;
    connected.modes.push_back(mode("1280x720", 1280, 720, 0));
    connected.modes.push_back(mode("1920x1080", 1920, 1080, kPreferred));
    snapshot.connectors.push_back(connected);

    kopms::DrmKmsSelection selection;
    std::string error;
    if (!expect(kopms::select_output(snapshot, &selection, &error),
                "connected output should be selected")) {
        return false;
    }
    if (!expect(selection.valid && selection.connector_id == 11 &&
                    selection.encoder_id == 200 && selection.crtc_id == 101 &&
                    selection.primary_plane_id == 301,
                "selected resource ids are incorrect")) {
        return false;
    }
    if (!expect(selection.mode.name == "1920x1080" && selection.mode.preferred(),
                "preferred mode was not selected")) {
        return false;
    }

    snapshot.connectors[1].connection = kUnknown;
    if (!expect(kopms::select_output(snapshot, &selection, &error),
                "unknown connector should be a fallback")) {
        return false;
    }

    snapshot.planes.clear();
    if (!expect(!kopms::select_output(snapshot, &selection, &error),
                "missing primary plane must be rejected")) {
        return false;
    }
    if (!expect(!error.empty(), "selection failure should explain the reason")) return false;

    if (!expect(!kopms::select_output(snapshot, nullptr, &error),
                "null selection target must be rejected")) {
        return false;
    }
    return true;
}

bool run_probe_failure_test() {
    kopms::DrmKmsSnapshot snapshot;
    std::string error;
    kopms::DrmKmsGuard guard("/dev/kopms-test-device-does-not-exist");
    if (!expect(!guard.discover(&snapshot, &error),
                "missing DRM device must not probe successfully")) {
        return false;
    }
    if (!expect(!error.empty(), "probe failure should contain a diagnostic")) return false;

    kopms::DrmKmsSession session;
    if (!expect(!session.open("", &error), "empty DRM session path was accepted")) {
        return false;
    }
    if (!expect(!session.open("/dev/kopms-test-device-does-not-exist", &error),
                "missing DRM session device was accepted")) {
        return false;
    }
    if (!expect(!session.is_open() && !session.is_master(),
                "failed DRM session left state behind")) {
        return false;
    }
    if (!expect(!session.atomic_enabled(),
                "failed DRM session left atomic capability state behind")) {
        return false;
    }
    return expect(!session.acquire_master(&error),
                  "closed DRM session acquired master");
}

bool run_atomic_validation_tests() {
    kopms::DrmKmsSnapshot snapshot;
    snapshot.atomic = true;
    snapshot.selection.valid = true;
    snapshot.selection.connector_id = 10;
    snapshot.selection.crtc_id = 20;
    snapshot.selection.primary_plane_id = 30;
    snapshot.selection.mode.clock = 148500;
    snapshot.selection.mode.width = 1920;
    snapshot.selection.mode.height = 1080;
    snapshot.selection.mode.refresh_hz = 60;
    snapshot.selection.mode.hsync_start = 2008;
    snapshot.selection.mode.hsync_end = 2052;
    snapshot.selection.mode.htotal = 2200;
    snapshot.selection.mode.vsync_start = 1084;
    snapshot.selection.mode.vsync_end = 1089;
    snapshot.selection.mode.vtotal = 1125;
    snapshot.properties.connector_crtc_id = 1;
    snapshot.properties.crtc_active = 2;
    snapshot.properties.crtc_mode_id = 3;
    snapshot.properties.plane_fb_id = 4;
    snapshot.properties.plane_crtc_id = 5;
    snapshot.properties.plane_crtc_x = 6;
    snapshot.properties.plane_crtc_y = 7;
    snapshot.properties.plane_crtc_w = 8;
    snapshot.properties.plane_crtc_h = 9;
    snapshot.properties.plane_src_x = 10;
    snapshot.properties.plane_src_y = 11;
    snapshot.properties.plane_src_w = 12;
    snapshot.properties.plane_src_h = 13;

    std::string error;
    if (!expect(kopms::validate_atomic_output(snapshot, 99, &error),
                "complete atomic output was rejected")) {
        return false;
    }

    kopms::DrmKmsSnapshot bad = snapshot;
    bad.atomic = false;
    if (!expect(!kopms::validate_atomic_output(bad, 99, &error),
                "non-atomic device passed validation")) {
        return false;
    }
    bad = snapshot;
    bad.properties.plane_fb_id = 0;
    if (!expect(!kopms::validate_atomic_output(bad, 99, &error),
                "incomplete plane properties passed validation")) {
        return false;
    }
    bad = snapshot;
    bad.selection.mode.clock = 0;
    if (!expect(!kopms::validate_atomic_output(bad, 99, &error),
                "incomplete mode timing passed validation")) {
        return false;
    }
    return expect(!kopms::validate_atomic_output(snapshot, 0, &error),
                  "zero framebuffer id passed validation");
}

}  // namespace

int main() {
    return run_selection_tests() && run_probe_failure_test() &&
                   run_atomic_validation_tests()
               ? 0
               : 1;
}
