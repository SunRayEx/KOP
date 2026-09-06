// P3-M2 libinput 防护骨架。默认不打开输入设备；调用方明确 start() 后
// 才会创建 udev context 并尝试绑定 seat。
#pragma once

#include <string>

struct libinput;

namespace kopms {

class LibinputGuard {
public:
    LibinputGuard() = default;
    ~LibinputGuard();

    LibinputGuard(const LibinputGuard&) = delete;
    LibinputGuard& operator=(const LibinputGuard&) = delete;

    bool start(const std::string& seat, std::string* error);
    void stop();
    bool dispatch(std::string* error);
    bool active() const { return context_ != nullptr; }

private:
    libinput* context_ = nullptr;
};

}  // namespace kopms
