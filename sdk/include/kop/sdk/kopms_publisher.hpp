// KOPMS 应用 SDK：把"Vulkan 导出 DMA-BUF + BUS2LAYER 提交 + release 泵 +
// 控制面窗口管理"封装成一个发布器 facade。
//
// 典型用法：
//   kop::sdk::PublisherOptions opts;
//   opts.bus_socket = "kop-0.bus";
//   kop::sdk::FramePublisher pub;
//   pub.connect(opts, &err);            // HELLO 能力协商 + 窗口绑定
//   pub.publish_cpu_frame(pixels, w, h, w * 4, pts_us, &err);  // 零拷贝上屏
//   pub.poll(5);                        // 泵 FRAME_RELEASE（回压回收）
//
// 线程模型：单线程使用（connect/publish/poll/控制面必须同一线程）。
#pragma once

#include <cstdint>
#include <string>

namespace kop {
namespace sdk {

struct PublisherOptions {
    std::string bus_socket;        // KOPMS-S BUS socket 名（XDG_RUNTIME_DIR 相对）
    uint64_t window_id = 1;        // 目标 CONTROL 窗口
    bool manage_window = true;     // connect 时执行 WINDOW_CREATE + WINDOW_ATTACH
    uint32_t max_in_flight = 3;    // 在飞帧上限（回压边界）
    uint32_t max_export_images = 8;
};

struct PublisherStats {
    uint64_t submitted = 0;
    uint64_t released = 0;
    uint64_t dropped = 0;
};

class FramePublisher {
public:
    FramePublisher() = default;
    ~FramePublisher();
    FramePublisher(const FramePublisher&) = delete;
    FramePublisher& operator=(const FramePublisher&) = delete;

    // 能力探测：当前环境（Vulkan 设备 + DMA-BUF 导出）是否可用。
    static bool available(std::string* reason);

    // 连接 + HELLO 能力协商（Handle/modifier/explicit-sync）+ 可选窗口绑定。
    bool connect(const PublisherOptions& options, std::string* error);
    void disconnect();
    bool connected() const;

    // 发布一帧 CPU RGBA 数据：内部上传为可导出 VkImage 并经 BUS 提交；
    // 返回后调用方即可复用该内存（已做引用计数转移）。
    bool publish_cpu_frame(const uint8_t* rgba, uint32_t width, uint32_t height,
                           uint32_t stride, int64_t pts_us, std::string* error);

    // 泵 FRAME_RELEASE（回压回收图像池）；timeout_ms 为单次等待上限。
    void poll(uint32_t timeout_ms);

    // 控制面操作（CONTROL lane；返回 ACK 状态是否 OK）
    bool create_window(uint64_t window_id, std::string* error);
    bool attach_window(uint64_t window_id, std::string* error);
    bool focus_window(uint64_t window_id, std::string* error);
    bool set_clipboard_owner(uint64_t window_id, std::string* error);
    bool set_ownership(uint64_t window_id, uint32_t ownership_state, std::string* error);

    PublisherStats stats() const;
    // 在飞帧数（达到 max_in_flight 时 publish 会阻塞泵 release——帧回压）
    size_t in_flight() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace sdk
}  // namespace kop
