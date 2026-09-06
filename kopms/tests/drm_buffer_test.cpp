#include <cstdio>
#include <string>

#include <unistd.h>

#include "drm_buffer.hpp"

namespace {

constexpr uint32_t kFormatXrgb8888 = 0x34325258;

kopms::DrmKmsSnapshot make_snapshot(bool modifiers) {
    kopms::DrmKmsSnapshot snapshot;
    snapshot.addfb2_modifiers = modifiers;
    snapshot.selection.valid = true;
    snapshot.selection.primary_plane_id = 40;
    snapshot.planes.push_back({40, 1, 1, {kFormatXrgb8888}});
    return snapshot;
}

kopms::DrmDmabufImportRequest make_request() {
    kopms::DrmDmabufImportRequest request;
    request.media_type = KOPAW_MEDIA_VIDEO;
    request.width = 640;
    request.height = 480;
    request.format = kFormatXrgb8888;
    request.plane_count = 1;
    request.planes[0].fd = 11;
    request.planes[0].stride = 640 * 4;
    return request;
}

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "drm-buffer test: %s\n", message);
    return condition;
}

bool run_validation_tests() {
    const kopms::DrmKmsSnapshot snapshot = make_snapshot(false);
    const kopms::DrmDmabufImportRequest valid = make_request();
    std::string error;
    if (!expect(kopms::validate_dmabuf_import(valid, 10, snapshot, &error),
                "valid linear request was rejected")) {
        return false;
    }

    kopms::DrmDmabufImportRequest bad = valid;
    bad.format = 0x36314752;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "unsupported format was accepted")) {
        return false;
    }
    bad = valid;
    bad.planes[0].fd = 10;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "DRM fd was accepted as a DMA-BUF source")) {
        return false;
    }
    bad = valid;
    bad.planes[0].stride = 0;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "zero stride was accepted")) {
        return false;
    }
    bad = valid;
    bad.planes[0].modifier = 1;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "modifier was accepted without ADD_FB2 modifier support")) {
        return false;
    }
    if (!expect(kopms::validate_dmabuf_import(bad, 10, make_snapshot(true), &error),
                "modifier was rejected despite ADD_FB2 modifier support")) {
        return false;
    }

    bad = valid;
    bad.acquire_fence_kind = KOPAW_SYNC_FENCE_TIMELINE;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "timeline fence was accepted by the KMS importer")) {
        return false;
    }
    bad = valid;
    bad.acquire_fence_kind = KOPAW_SYNC_FENCE_NONE;
    bad.acquire_fence_fd = 12;
    if (!expect(!kopms::validate_dmabuf_import(bad, 10, snapshot, &error),
                "unexpected fence fd was accepted")) {
        return false;
    }

    kopms::DrmKmsSnapshot no_selection = snapshot;
    no_selection.selection = {};
    if (!expect(!kopms::validate_dmabuf_import(valid, 10, no_selection, &error),
                "request without a selected plane was accepted")) {
        return false;
    }

    kopms::DrmImportedBuffer output;
    if (!expect(!kopms::import_dmabuf_frame(-1, valid, snapshot, &output, 0, &error),
                "import with an invalid DRM fd succeeded")) {
        return false;
    }
    return expect(!output.valid(), "failed import left an active output");
}

bool run_fence_tests() {
    int pipe_fds[2] = {-1, -1};
    if (!expect(::pipe(pipe_fds) == 0, "cannot create fence test pipe")) return false;

    kopms::DrmDmabufImportRequest request;
    request.acquire_fence_kind = KOPAW_SYNC_FENCE_FD;
    request.acquire_fence_fd = pipe_fds[0];
    std::string error;
    if (!expect(!kopms::wait_for_acquire_fence(request, 0, &error),
                "unsignaled fence did not time out")) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    const char signal = 1;
    if (!expect(::write(pipe_fds[1], &signal, sizeof(signal)) == sizeof(signal),
                "cannot signal fence test pipe")) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    const bool signaled = kopms::wait_for_acquire_fence(request, 100, &error);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return expect(signaled, "signaled fence was not accepted");
}

}  // namespace

int main() { return run_validation_tests() && run_fence_tests() ? 0 : 1; }
