#include "kopms_sink_node.hpp"

#include <cstring>
#include <unordered_map>

#include "kop/log.h"
#include "kop/time.h"
#include "frame.hpp"
#include "frame_bridge.h"
#include "kopms_client.h"
#include "kopms_protocol.h"

namespace kopaw {

static const char* kTag = "kopms-sink";

namespace {

// KOPAW 导出格式 R8G8B8A8 对应的 DRM fourcc（ABGR8888）。
constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241u;

}  // namespace

struct KopmsSinkNode::Impl {
    kopms::KopmsClient client;
};

// BUS 侧 descriptor 与 KopawFrame 引用计数的桥：retain/release 直接映射到
// GPU 帧的引用；Submission 由 sink 在 release 时回收。
struct KopmsSinkNode::Submission {
    KopmsFrameDescriptor desc{};
    KopawFrame* gpu_frame = nullptr;
    KopmsSinkNode* self = nullptr;
};

void KopmsSinkNode::desc_retain(KopmsFrameDescriptor* desc) {
    auto* sub = static_cast<Submission*>(desc->user_data);
    if (sub && sub->gpu_frame && sub->gpu_frame->retain) {
        sub->gpu_frame->retain(sub->gpu_frame);
    }
}

void KopmsSinkNode::desc_release(KopmsFrameDescriptor* desc) {
    auto* sub = static_cast<Submission*>(desc->user_data);
    if (sub && sub->self) {
        sub->self->free_submission(sub);
    }
}

KopmsSinkNode::KopmsSinkNode(Options options)
    : options_(std::move(options)), impl_(std::make_unique<Impl>()) {}

KopmsSinkNode::~KopmsSinkNode() { disconnect(); }

void KopmsSinkNode::free_submission(Submission* sub) {
    if (!sub) return;
    pending_.erase(reinterpret_cast<uintptr_t>(sub));
    if (sub->gpu_frame && sub->gpu_frame->release) {
        // 两份引用在此归还：client submit 时的 retain（FRAME_RELEASE 对应）
        // 与 sink 自持的原始导出引用。引用归零 → fd 关闭 → 图像回池。
        sub->gpu_frame->release(sub->gpu_frame);
        sub->gpu_frame->release(sub->gpu_frame);
    }
    delete sub;
    ++frames_released_;
}

bool KopmsSinkNode::connect(std::string* error) {
    disconnect();
    if (!exporter_.inited()) {
        if (exporter_.init(options_.max_export_images, error)) return false;
    }
    const uint64_t caps = KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                          KOPMS_PROTOCOL_CAP_HANDLE_FRAMES |
                          KOPMS_PROTOCOL_CAP_MODIFIERS |
                          KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC |
                          KOPMS_PROTOCOL_CAP_CONTROL_STATE;
    if (!impl_->client.connect(options_.bus_socket, error) ||
        !impl_->client.hello(caps, error)) {
        return false;
    }
    if ((impl_->client.capabilities() &
         (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) !=
        (KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE)) {
        if (error) *error = "KOPMS-S 未协商 DMA-BUF/FRAME_RELEASE 能力";
        return false;
    }
    if (options_.manage_window &&
        (impl_->client.capabilities() & KOPMS_PROTOCOL_CAP_CONTROL_STATE) != 0) {
        if (!setup_window(error)) return false;
    }
    connected_ = true;
    KOP_LOG_INFO(kTag, "已连接 KOPMS-S（bus=%s window=%llu caps=0x%llx）",
                 options_.bus_socket.c_str(),
                 static_cast<unsigned long long>(options_.window_id),
                 static_cast<unsigned long long>(impl_->client.capabilities()));
    return true;
}

bool KopmsSinkNode::setup_window(std::string* error) {
    KopmsControlCommandPayload command{};
    command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
    command.operation = KOPMS_CONTROL_WINDOW_CREATE;
    command.object_id = options_.window_id;
    uint64_t seq = 0;
    KopmsControlAckPayload ack{};
    if (!impl_->client.send_control(command, {}, &seq, error) ||
        !impl_->client.wait_for_control_ack(seq, 1000, &ack, error)) {
        // 窗口已存在等情况：attached 才是硬性要求
    }
    command.operation = KOPMS_CONTROL_WINDOW_ATTACH;
    command.value = 1;
    if (!impl_->client.send_control(command, {}, &seq, error) ||
        !impl_->client.wait_for_control_ack(seq, 1000, &ack, error) ||
        ack.status != KOPMS_CONTROL_STATUS_OK) {
        if (error) {
            *error = "WINDOW_ATTACH 失败（status=" +
                     std::to_string(ack.status) + "）";
        }
        return false;
    }
    return true;
}

void KopmsSinkNode::disconnect() {
    if (!impl_) return;
    impl_->client.disconnect();
    // disconnect() 已对全部在飞 descriptor 调 release → free_submission。
    connected_ = false;
}

void KopmsSinkNode::pump_releases(int timeout_ms) {
    std::string ignored;
    impl_->client.dispatch(timeout_ms, &ignored);
}

void KopmsSinkNode::drain(bool wait_all) {
    // 泵 FRAME_RELEASE 直到在飞清空（wait_all）或仅推进一轮。
    std::string ignored;
    while (!pending_.empty()) {
        if (!impl_->client.dispatch(wait_all ? 20 : 0, &ignored)) break;
    }
}

int32_t KopmsSinkNode::send_impl(KopawFrame* frame) {
    if (!frame) return KOPAW_E_INVALID;
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        drain(true);
        frame->release(frame);
        if (graph_ && !sink_done_) {
            kopaw_node_sink_done(graph_, node_id_);
            sink_done_ = true;
        }
        KOP_LOG_INFO(kTag, "EOS：在飞 %zu 帧全部释放，sink 记账完成",
                     pending_.size());
        return KOPAW_OK;
    }
    if (!connected_) {
        frame->release(frame);
        ++frames_dropped_;
        return KOPAW_OK;
    }

    // 回压：在飞帧满时阻塞泵 release（引擎 send 阻塞 → 上游减速）。
    // 等待有界（500ms）：消费者长时间不回 release 时丢弃本帧而不是挂死，
    // 图停止流程也不会被 sink 卡住。
    std::string error;
    {
        const int64_t deadline = kop::steady_us() + 500'000;
        while (pending_.size() >= options_.max_in_flight) {
            std::string pump_error;
            if (impl_->client.dispatch(10, &pump_error)) continue;
            if (kop::steady_us() >= deadline) {
                KOP_LOG_WARN(kTag, "在飞 %zu 帧等待 release 超时，丢弃本帧",
                             pending_.size());
                frame->release(frame);
                ++frames_dropped_;
                return KOPAW_OK;
            }
        }
    }
    // 每帧先推进一轮已完成释放，缩短图像池驻留时间。
    pump_releases(0);

    KopawFrame* gpu = exporter_.export_cpu_frame(frame, &error);
    if (!gpu) {
        KOP_LOG_WARN(kTag, "导出失败（%s），丢弃本帧", error.c_str());
        frame->release(frame);
        ++frames_dropped_;
        return KOPAW_OK;
    }

    auto* sub = new Submission();
    sub->gpu_frame = gpu;
    sub->self = this;
    KopmsFrameDescriptor& d = sub->desc;
    d.struct_size = sizeof(d);
    d.version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    d.media_type = KOPAW_MEDIA_VIDEO;
    d.width = gpu->format.video.width;
    d.height = gpu->format.video.height;
    d.format = kDrmFormatAbgr8888;
    d.memory_type = gpu->memory_type;
    d.plane_count = gpu->plane_count;
    for (uint32_t i = 0; i < gpu->plane_count && i < KOPAW_MAX_DMABUF_PLANES; ++i) {
        d.planes[i] = gpu->planes[i];
    }
    d.acquire_fence = gpu->acquire_fence;
    d.dma_buf_handle = static_cast<uint32_t>(gpu->planes[0].fd);
    d.cpu_size = 0;
    d.user_data = sub;
    d.retain = &KopmsSinkNode::desc_retain;
    d.release = &KopmsSinkNode::desc_release;
    d.pts = gpu->pts;
    d.dts = gpu->dts;

    uint32_t frame_id = 0;
    if (!impl_->client.submit(&sub->desc, &frame_id, &error)) {
        // submit 失败：client 已对 descriptor 执行 release → free_submission
        // 回收了自持引用并删除 sub；这里只记账。
        KOP_LOG_WARN(kTag, "FRAME_SUBMIT 失败（%s）", error.c_str());
        ++frames_dropped_;
    } else {
        pending_.emplace(reinterpret_cast<uintptr_t>(sub), sub);
        ++frames_submitted_;
    }
    frame->release(frame);
    return KOPAW_OK;
}

KopawNodeDesc KopmsSinkNode::desc() {
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "kopms_sink";
    d.user_data = this;
    d.outputs = 0;
    d.inputs = 1;
    d.queue_capacity = 4;
    d.is_sink = 1;
    d.self_driven = 0;
    static KopawNodeVTable vt = [] {
        KopawNodeVTable v{};
        v.struct_size = sizeof(v);
        v.run = nullptr;
        v.send = [](void* user, KopawFrame* frame) -> int32_t {
            return static_cast<KopmsSinkNode*>(user)->send_impl(frame);
        };
        v.stop = [](void*) {};
        v.destroy = [](void* user) { delete static_cast<KopmsSinkNode*>(user); };
        return v;
    }();
    d.vtable = &vt;
    return d;
}

}  // namespace kopaw
