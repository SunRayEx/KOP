#include "libinput_guard.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#ifdef KOPMS_HAVE_LIBINPUT
#include <libinput.h>
#endif

#include "kop/log.h"

namespace kopms {

#ifdef KOPMS_HAVE_LIBINPUT
namespace {

int open_restricted(const char* path, int flags, void*) {
    return open(path, flags | O_CLOEXEC);
}

void close_restricted(int fd, void*) { close(fd); }

const struct libinput_interface kInterface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

}  // namespace
#endif

LibinputGuard::~LibinputGuard() { stop(); }

bool LibinputGuard::start(const std::string& seat, std::string* error) {
    stop();
#ifndef KOPMS_HAVE_LIBINPUT
    if (error) *error = "libinput 不可用，输入设备保持禁用";
    return false;
#else
    if (seat.empty()) {
        if (error) *error = "libinput seat 为空";
        return false;
    }
    context_ = libinput_udev_create_context(&kInterface, nullptr, nullptr);
    if (!context_) {
        if (error) *error = "创建 libinput udev context 失败";
        return false;
    }
    if (libinput_udev_assign_seat(context_, seat.c_str()) < 0) {
        if (error) *error = "绑定 libinput seat 失败: " + seat;
        stop();
        return false;
    }
    KOP_LOG_INFO("kopms-input", "libinput 已绑定 seat=%s", seat.c_str());
    return true;
#endif
}

void LibinputGuard::stop() {
#ifdef KOPMS_HAVE_LIBINPUT
    if (context_) libinput_unref(context_);
#endif
    context_ = nullptr;
}

bool LibinputGuard::dispatch(std::string* error) {
#ifndef KOPMS_HAVE_LIBINPUT
    if (error) *error = "libinput 不可用";
    return false;
#else
    if (!context_) {
        if (error) *error = "libinput 尚未启动";
        return false;
    }
    if (libinput_dispatch(context_) < 0) {
        if (error) *error = "libinput dispatch 失败: ";
        if (error) *error += std::strerror(errno);
        return false;
    }
    // Input injection is deliberately not enabled in this guard skeleton.
    // Drain events so a protected compositor cannot accumulate unhandled input.
    while (libinput_event* event = libinput_get_event(context_)) {
        libinput_event_destroy(event);
    }
    return true;
#endif
}

}  // namespace kopms
