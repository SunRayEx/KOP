// KOPAW 网络源/汇聚节点实现。
#include "net_channel.hpp"
#include "net_source_node.hpp"
#include "net_sink_node.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "frame.hpp"
#include "frame_envelope.hpp"
#include "kop/log.h"
#include "kopaw_abi.h"
#include "kopnet/adapters.hpp"
#include "kopnet/resilient.hpp"

namespace kopaw {

namespace {

static const char* kTag = "kopnet-node";

KopawNodeVTable make_source_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.run = [](void* user) -> int32_t { return static_cast<NetSourceNode*>(user)->run_impl(); };
    vt.stop = [](void* user) { static_cast<NetSourceNode*>(user)->request_stop(); };
    vt.destroy = [](void* user) { delete static_cast<NetSourceNode*>(user); };
    vt.bind_output = [](void* user, uint32_t port, KopawOutput output) {
        if (port == 0) static_cast<NetSourceNode*>(user)->set_output(output);
    };
    return vt;
}

const KopawNodeVTable kSourceVTable = make_source_vtable();

KopawNodeVTable make_sink_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* frame) -> int32_t {
        return static_cast<NetSinkNode*>(user)->send_impl(frame);
    };
    vt.stop = [](void* user) { static_cast<NetSinkNode*>(user)->request_stop(); };
    vt.destroy = [](void* user) { delete static_cast<NetSinkNode*>(user); };
    return vt;
}

const KopawNodeVTable kSinkVTable = make_sink_vtable();

// 拨号参数（两个节点的 Options 公共部分）
struct ChannelParams {
    std::string endpoint;
    std::vector<std::string> fallbacks;
    bool auto_reconnect = false;
    uint32_t base_delay_ms = 500;
    uint32_t max_delay_ms = 5000;
    std::string kind;
    kopnet::ChannelMode mode = kopnet::ChannelMode::Stream;
};

class PlainChannel : public NetChannel {
public:
    PlainChannel(std::unique_ptr<kopnet::TunnelSession> session, uint32_t id)
        : session_(std::move(session)), id_(id) {}

    kopnet::SendStatus send(const uint8_t* data, size_t len, const std::vector<int>* fds,
                            int timeout_ms) override {
        return session_->send(id_, data, len, fds, timeout_ms);
    }
    kopnet::RecvStatus recv(std::vector<uint8_t>* data, std::vector<int>* fds,
                            int timeout_ms) override {
        return session_->recv(id_, data, fds, timeout_ms);
    }
    bool alive() const override { return session_->alive(); }
    bool supports_fds() const override { return session_->transport()->supports_fds(); }
    void stop() override { session_->stop(); }

private:
    std::unique_ptr<kopnet::TunnelSession> session_;
    uint32_t id_;
};

class ResilientChannel : public NetChannel {
public:
    ResilientChannel(std::unique_ptr<kopnet::ResilientSession> session, uint32_t id)
        : session_(std::move(session)), id_(id) {}

    kopnet::SendStatus send(const uint8_t* data, size_t len, const std::vector<int>* fds,
                            int timeout_ms) override {
        return session_->send(id_, data, len, fds, timeout_ms);
    }
    kopnet::RecvStatus recv(std::vector<uint8_t>* data, std::vector<int>* fds,
                            int timeout_ms) override {
        return session_->recv(id_, data, fds, timeout_ms);
    }
    // 重连线程未退出即视为会话仍可工作（断开期间 recv 会自动挂起等待重连）
    bool alive() const override { return !session_->reconnect_done(); }
    bool supports_fds() const override { return session_->supports_fds(); }
    void stop() override { session_->stop(); }

private:
    std::unique_ptr<kopnet::ResilientSession> session_;
    uint32_t id_;
};

std::unique_ptr<NetChannel> dial_channel(const ChannelParams& p, std::string* error) {
    if (p.auto_reconnect) {
        kopnet::ResilientSession::Options opts;
        opts.endpoints.push_back(p.endpoint);
        for (const auto& f : p.fallbacks) opts.endpoints.push_back(f);
        opts.base_delay_ms = p.base_delay_ms;
        opts.max_delay_ms = p.max_delay_ms;
        opts.connect_timeout_ms = 5000;
        auto session = std::make_unique<kopnet::ResilientSession>(opts);
        if (!session->start(error)) return nullptr;
        // 逻辑通道号跨重连保持；未连接时只是注册，连接线程会在握手后打开
        const uint32_t id = session->open_channel(p.kind, p.mode, error);
        if (id == 0) {
            session->stop();
            return nullptr;
        }
        return std::make_unique<ResilientChannel>(std::move(session), id);
    }
    std::string dial_error;
    auto session =
        kopnet::tunnel_dial(p.endpoint, kopnet::TunnelSession::Options(), &dial_error);
    if (!session) {
        if (error) *error = "连接端点失败: " + dial_error;
        return nullptr;
    }
    const uint32_t id = session->open_channel(p.kind, p.mode, 5000, error);
    if (id == 0) {
        session->stop();
        return nullptr;
    }
    return std::make_unique<PlainChannel>(std::move(session), id);
}

}  // namespace

// 把一条隧道消息重建为一帧并投递到图输出。消息为空时丢弃；
// 解析失败时关闭附带的 fds 并返回 false（流已不可恢复）。
bool emit_tunnel_frame(KopawGraph* graph, KopawOutput out,
                       const std::vector<uint8_t>& data,
                       const std::vector<int>& fds, uint64_t* frames_emitted) {
    if (data.empty()) {
        for (int fd : fds) ::close(fd);
        return true;
    }
    // make_frame 提供 struct_size / retain / release / dma_fd=-1 / planes fd=-1
    // 的安全初值，反序列化只覆盖线上字段
    std::unique_ptr<kopaw::OwnedFrame> holder(
        kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0));
    std::vector<uint8_t> payload;
    std::string env_error;
    if (!kopnet::deserialize_frame(data.data(), data.size(), fds.data(),
                                   fds.size(), holder->ptr(), &payload, &env_error)) {
        KOP_LOG_ERROR(kTag, "帧信封解析失败（%s）", env_error.c_str());
        for (int fd : fds) ::close(fd);
        return false;
    }
    if (holder->frame.memory_type == KOPAW_MEMORY_CPU && !payload.empty()) {
        holder->storage = std::move(payload);
        holder->frame.dma_buf_handle =
            kopaw::handle_from_cpu_address(holder->storage.data());
        holder->frame.size = holder->storage.size();
    }
    // emit 的任何返回路径（未连接/已停止/tee 校验）都已释放帧
    kopaw_graph_emit(graph, out, holder->ptr());
    holder.release();
    if (frames_emitted) ++*frames_emitted;
    return true;
}

// ---------------------------------------------------------------------------
// NetSourceNode
// ---------------------------------------------------------------------------

NetSourceNode::NetSourceNode(Options options) : options_(std::move(options)) {}

NetSourceNode::~NetSourceNode() {
    if (channel_) channel_->stop();
}

bool NetSourceNode::open(std::string* error) {
    if (channel_) return true;
    ChannelParams p;
    p.endpoint = options_.endpoint;
    p.fallbacks = options_.fallback_endpoints;
    p.auto_reconnect = options_.auto_reconnect;
    p.base_delay_ms = options_.reconnect_base_delay_ms;
    p.max_delay_ms = options_.reconnect_max_delay_ms;
    p.kind = options_.kind;
    p.mode = options_.mode;
    channel_ = dial_channel(p, error);
    return channel_ != nullptr;
}

KopawNodeDesc NetSourceNode::desc(KopawGraph* g) {
    graph_ = g;
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "net-source";
    d.user_data = this;
    d.outputs = 1;
    d.inputs = 0;
    d.queue_capacity = options_.queue_capacity;
    d.is_sink = 0;
    d.self_driven = 1;
    d.vtable = &kSourceVTable;
    return d;
}

int32_t NetSourceNode::run_impl() {
    if (!channel_) return KOPAW_E_INVALID;
    int32_t result = KOPAW_OK;
    const int recv_timeout = 200;
    int64_t idle_since = 0;
    const int64_t idle_limit_us =
        static_cast<int64_t>(options_.idle_timeout_ms) * 1000;

    while (!stopped_.load(std::memory_order_acquire)) {
        std::vector<uint8_t> data;
        std::vector<int> fds;
        kopnet::RecvStatus st = channel_->recv(&data, &fds, recv_timeout);
        if (st == kopnet::RecvStatus::Timeout) {
            if (!channel_->alive()) {
                result = KOPAW_E_GENERIC;
                break;
            }
            if (idle_limit_us > 0) {
                int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
                if (idle_since == 0) idle_since = now;
                if (now - idle_since >= idle_limit_us) {
                    KOP_LOG_ERROR(kTag, "网络源 %ums 无数据", options_.idle_timeout_ms);
                    result = KOPAW_E_TIMEOUT;
                    break;
                }
            }
            continue;
        }
        if (st == kopnet::RecvStatus::Closed) {
            KOP_LOG_INFO(kTag, "网络通道关闭，源结束");
            result = KOPAW_E_EOS;
            break;
        }
        if (st != kopnet::RecvStatus::Ok) {
            KOP_LOG_ERROR(kTag, "网络接收错误");
            result = KOPAW_E_GENERIC;
            break;
        }
        idle_since = 0;
        if (!emit_tunnel_frame(graph_, out_, data, fds, &frames_emitted_)) {
            result = KOPAW_E_GENERIC;
            break;
        }
    }
    if (channel_) {
        // 通知下游 EOS
        OwnedFrame* eos = make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0);
        eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(graph_, out_, eos->ptr());
        channel_->stop();
    }
    return result;
}

// ---------------------------------------------------------------------------
// NetSinkNode
// ---------------------------------------------------------------------------

NetSinkNode::NetSinkNode(Options options) : options_(std::move(options)) {}

NetSinkNode::~NetSinkNode() {
    if (channel_) channel_->stop();
}

bool NetSinkNode::open(std::string* error) {
    if (channel_) return true;
    ChannelParams p;
    p.endpoint = options_.endpoint;
    p.fallbacks = options_.fallback_endpoints;
    p.auto_reconnect = options_.auto_reconnect;
    p.base_delay_ms = options_.reconnect_base_delay_ms;
    p.max_delay_ms = options_.reconnect_max_delay_ms;
    p.kind = options_.kind;
    p.mode = options_.mode;
    channel_ = dial_channel(p, error);
    return channel_ != nullptr;
}

KopawNodeDesc NetSinkNode::desc() {
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "net-sink";
    d.user_data = this;
    d.outputs = 0;
    d.inputs = 1;
    d.queue_capacity = options_.queue_capacity;
    d.is_sink = 1;
    d.self_driven = 0;
    d.vtable = &kSinkVTable;
    return d;
}

int32_t NetSinkNode::send_impl(KopawFrame* frame) {
    if (!frame) return KOPAW_E_INVALID;
    if (!channel_) {
        if (frame->release) frame->release(frame);
        return KOPAW_E_INVALID;
    }
    if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
        if (frame->release) frame->release(frame);
        if (graph_) kopaw_node_sink_done(graph_, node_id_);
        return KOPAW_OK;
    }
    if (frame->memory_type != KOPAW_MEMORY_CPU &&
        frame->memory_type != KOPAW_MEMORY_DMABUF) {
        // VULKAN 等外部内存：上层应先转成 DMABUF
        ++frames_dropped_;
        if (frame->release) frame->release(frame);
        KOP_LOG_WARN(kTag, "不支持的帧内存类型 %u，丢弃", frame->memory_type);
        return KOPAW_OK;
    }

    std::vector<uint8_t> wire;
    uint32_t plane_fd_count = 0;
    bool send_fence_fd = false;
    std::string env_error;
    if (!kopnet::serialize_frame(*frame, &wire, &plane_fd_count, &send_fence_fd,
                                 &env_error)) {
        ++frames_dropped_;
        KOP_LOG_WARN(kTag, "帧序列化失败（%s），丢帧", env_error.c_str());
        if (frame->release) frame->release(frame);
        return KOPAW_OK;
    }

    std::vector<int> owned_fds;
    const bool need_fds = plane_fd_count > 0 || send_fence_fd;
    if (need_fds && !channel_->supports_fds()) {
        // 该传输不透传 fd：零拷贝路径不可用，直接丢弃
        ++frames_dropped_;
        if (frame->release) frame->release(frame);
        KOP_LOG_WARN(kTag, "当前传输不支持 fd 透传，丢弃 DMA-BUF 帧");
        return KOPAW_OK;
    }
    if (need_fds) {
        // fd 必须 dup：帧释放会关闭原 fd，而隧道发送是异步的；顺序与线上
        // 约定一致——先 plane fd（按 plane 序），最后 fence fd
        const uint32_t planes = frame->plane_count;
        for (uint32_t i = 0; i < planes && owned_fds.size() < plane_fd_count; ++i) {
            if (frame->planes[i].fd >= 0) {
                const int dup = ::dup(frame->planes[i].fd);
                if (dup < 0) {
                    ++frames_dropped_;
                    for (int fd : owned_fds) ::close(fd);
                    if (frame->release) frame->release(frame);
                    KOP_LOG_WARN(kTag, "dup 平面 fd 失败，丢帧");
                    return KOPAW_OK;
                }
                owned_fds.push_back(dup);
            }
        }
        if (send_fence_fd) {
            const int dup = ::dup(frame->acquire_fence.fd);
            if (dup < 0) {
                ++frames_dropped_;
                for (int fd : owned_fds) ::close(fd);
                if (frame->release) frame->release(frame);
                KOP_LOG_WARN(kTag, "dup fence fd 失败，丢帧");
                return KOPAW_OK;
            }
            owned_fds.push_back(dup);
        }
    }

    const std::vector<int>* fdsptr = owned_fds.empty() ? nullptr : &owned_fds;
    kopnet::SendStatus st =
        channel_->send(wire.data(), wire.size(), fdsptr,
                       static_cast<int>(options_.send_timeout_ms));
    if (st == kopnet::SendStatus::Ok) {
        ++frames_sent_;
        // fd 所有权已移交队列（接收方负责关闭），本地副本不可再动
    } else {
        ++frames_dropped_;
        for (int fd : owned_fds) ::close(fd);
        KOP_LOG_WARN(kTag, "网络发送失败，丢帧");
    }
    if (frame->release) frame->release(frame);
    return KOPAW_OK;
}

}  // namespace kopaw
