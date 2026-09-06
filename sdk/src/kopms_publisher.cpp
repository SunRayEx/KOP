#include "kop/sdk/kopms_publisher.hpp"

#include <cstring>
#include <unordered_map>

#include "frame.hpp"
#include "frame_bridge.h"
#include "kop/log.h"
#include "kopms_client.h"
#include "kopms_protocol.h"
#include "render/vulkan/vk_dma_export.hpp"

namespace kop {
namespace sdk {

static const char* kTag = "kop-sdk-pub";

namespace {
// KOPAW 导出格式 R8G8B8A8 的 DRM fourcc（ABGR8888）
constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241u;
constexpr uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF |
                                 KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                                 KOPMS_PROTOCOL_CAP_HANDLE_FRAMES |
                                 KOPMS_PROTOCOL_CAP_MODIFIERS |
                                 KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC |
                                 KOPMS_PROTOCOL_CAP_CONTROL_STATE;
}  // namespace

bool FramePublisher::available(std::string* reason) {
    return kopaw::VulkanDmabufExporter::export_supported(reason);
}

struct FramePublisher::Impl {
    PublisherOptions options;
    kopaw::VulkanDmabufExporter exporter;
    kopms::KopmsClient client;
    bool connected = false;

    // 在飞提交：FRAME_RELEASE 沿 descriptor.release 归还（client retain +
    // SDK 自持引用一并释放，图像回池、fd 关闭）。
    struct Submission {
        KopmsFrameDescriptor desc{};
        KopawFrame* gpu = nullptr;
        Impl* self = nullptr;
    };
    std::unordered_map<uintptr_t, Submission*> pending;
    PublisherStats stats;

    static void desc_retain(KopmsFrameDescriptor* d) {
        auto* sub = static_cast<Submission*>(d->user_data);
        if (sub && sub->gpu && sub->gpu->retain) sub->gpu->retain(sub->gpu);
    }
    static void desc_release(KopmsFrameDescriptor* d) {
        auto* sub = static_cast<Submission*>(d->user_data);
        if (sub && sub->self) sub->self->free_submission(sub);
    }
    void free_submission(Submission* sub) {
        pending.erase(reinterpret_cast<uintptr_t>(sub));
        if (sub->gpu && sub->gpu->release) {
            sub->gpu->release(sub->gpu);  // client retain
            sub->gpu->release(sub->gpu);  // SDK 自持引用
        }
        delete sub;
        ++stats.released;
    }

    bool control(uint32_t operation, uint64_t window_id, uint64_t value,
                 std::string* error) {
        KopmsControlCommandPayload command{};
        command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
        command.operation = operation;
        command.object_id = window_id;
        command.value = value;
        uint64_t seq = 0;
        KopmsControlAckPayload ack{};
        if (!client.send_control(command, {}, &seq, error) ||
            !client.wait_for_control_ack(seq, 2000, &ack, error)) {
            return false;
        }
        if (ack.status != KOPMS_CONTROL_STATUS_OK) {
            if (error) {
                *error = "CONTROL status=" + std::to_string(ack.status);
            }
            return false;
        }
        return true;
    }
};

FramePublisher::~FramePublisher() { disconnect(); }

bool FramePublisher::connect(const PublisherOptions& options, std::string* error) {
    disconnect();
    Impl* impl = new Impl();
    impl_ = impl;
    impl->options = options;
    // kopaw 约定：init 返回 true = 失败
    if (impl->exporter.init(options.max_export_images, error)) {
        return false;
    }
    if (!impl->client.connect(options.bus_socket, error) ||
        !impl->client.hello(kClientCaps, error)) {
        return false;
    }
    if ((impl->client.capabilities() &
         (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) !=
        (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) {
        if (error) *error = "KOPMS-S 未协商 DMA-BUF 能力";
        return false;
    }
    impl->connected = true;
    if (options.manage_window) {
        if (!create_window(options.window_id, error) ||
            !attach_window(options.window_id, error)) {
            impl->connected = false;
            return false;
        }
    }
    return true;
}

void FramePublisher::disconnect() {
    if (!impl_) return;
    impl_->client.disconnect();  // 触发全部在飞 descriptor 的 release 回调
    impl_->exporter.shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool FramePublisher::connected() const { return impl_ && impl_->connected; }

bool FramePublisher::publish_cpu_frame(const uint8_t* rgba, uint32_t width,
                                       uint32_t height, uint32_t stride,
                                       int64_t pts_us, std::string* error) {
    if (!impl_ || !impl_->connected) {
        if (error) *error = "publisher 未连接";
        return false;
    }
    Impl& impl = *impl_;
    if (!rgba || width == 0 || height == 0 || stride < width * 4) {
        if (error) *error = "帧参数无效";
        return false;
    }
    // 回压：在飞满时泵 release（有界等待，超时放弃本帧）
    {
        const int64_t deadline =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count() + 500;
        while (impl.pending.size() >= impl.options.max_in_flight) {
            std::string ignored;
            if (impl.client.dispatch(10, &ignored)) continue;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count() > deadline) {
                if (error) *error = "回压超时（FRAME_RELEASE 未返回）";
                ++impl.stats.dropped;
                return false;
            }
        }
    }
    // CPU 帧组装（逐行拷贝，容忍调用方 stride ≠ width*4）
    kopaw::OwnedFrame* cpu =
        kopaw::make_frame(KOPAW_MEDIA_VIDEO, pts_us, pts_us,
                          static_cast<size_t>(width) * height * 4);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(cpu->data() + static_cast<size_t>(row) * width * 4,
                    rgba + static_cast<size_t>(row) * stride, width * 4);
    }
    cpu->frame.format.video.width = width;
    cpu->frame.format.video.height = height;
    cpu->frame.stride = width * 4;

    KopawFrame* gpu = impl.exporter.export_cpu_frame(&cpu->frame, error);
    cpu->release_cb(&cpu->frame);
    if (!gpu) {
        ++impl.stats.dropped;
        if (error) *error = "导出失败: " + (error ? *error : "");
        return false;
    }

    auto* sub = new Impl::Submission();
    sub->gpu = gpu;
    sub->self = &impl;
    KopmsFrameDescriptor& d = sub->desc;
    d.struct_size = sizeof(d);
    d.version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    d.media_type = KOPAW_MEDIA_VIDEO;
    d.width = width;
    d.height = height;
    d.format = kDrmFormatAbgr8888;
    d.memory_type = gpu->memory_type;
    d.plane_count = gpu->plane_count;
    d.planes[0] = gpu->planes[0];
    d.acquire_fence = gpu->acquire_fence;
    d.dma_buf_handle = static_cast<uint32_t>(gpu->planes[0].fd);
    d.user_data = sub;
    d.retain = &Impl::desc_retain;
    d.release = &Impl::desc_release;
    d.pts = pts_us;

    uint32_t frame_id = 0;
    if (!impl.client.submit(&d, &frame_id, error)) {
        // submit 失败：client 已触发 desc.release（回收自持引用）
        ++impl.stats.dropped;
        return false;
    }
    impl.pending.emplace(reinterpret_cast<uintptr_t>(sub), sub);
    ++impl.stats.submitted;
    return true;
}

void FramePublisher::poll(uint32_t timeout_ms) {
    if (!impl_) return;
    std::string ignored;
    if (timeout_ms > 0) {
        impl_->client.dispatch(timeout_ms, &ignored);
    }
    for (int i = 0; i < 8 && impl_->client.dispatch(0, &ignored); ++i) {
    }
}

bool FramePublisher::create_window(uint64_t window_id, std::string* error) {
    return impl_ && impl_->control(KOPMS_CONTROL_WINDOW_CREATE, window_id, 0, error);
}

bool FramePublisher::attach_window(uint64_t window_id, std::string* error) {
    return impl_ && impl_->control(KOPMS_CONTROL_WINDOW_ATTACH, window_id, 1, error);
}

bool FramePublisher::focus_window(uint64_t window_id, std::string* error) {
    return impl_ && impl_->control(KOPMS_CONTROL_FOCUS_SET, window_id, 0, error);
}

bool FramePublisher::set_clipboard_owner(uint64_t window_id, std::string* error) {
    return impl_ &&
           impl_->control(KOPMS_CONTROL_CLIPBOARD_SET_OWNER, window_id, 0, error);
}

bool FramePublisher::set_ownership(uint64_t window_id, uint32_t ownership_state,
                                   std::string* error) {
    return impl_ &&
           impl_->control(KOPMS_CONTROL_OWNERSHIP_SET, window_id, ownership_state,
                          error);
}

PublisherStats FramePublisher::stats() const {
    return impl_ ? impl_->stats : PublisherStats{};
}

size_t FramePublisher::in_flight() const {
    return impl_ ? impl_->pending.size() : 0;
}

}  // namespace sdk
}  // namespace kop
