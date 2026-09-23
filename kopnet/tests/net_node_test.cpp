// KOPNET 网络节点测试：把 net-source / net-sink 接入真实 KOPAW 图，
// 经隧道端到端验证帧流转：
//   1) 下行（TCP 隧道）：服务端按帧信封推送，net-source 重建 CPU 帧，collector 收齐
//   2) 上行（TCP 隧道）：packet-source → net-sink，服务端解析信封核对载荷与元数据
//   3) DMA-BUF 透传（Unix 隧道）：双平面 memfd 帧 + acquire fence fd 经
//      SCM_RIGHTS 透传，服务端 mmap 两个平面核对内容——验证 fd 与平面对齐
#include "net_sink_node.hpp"
#include "net_source_node.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "frame.hpp"
#include "frame_envelope.hpp"
#include "kopaw_abi.h"
#include "kopnet/adapters.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// 按帧重建后的可核对信息
struct FrameInfo {
    std::vector<uint8_t> payload;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint8_t memory_type = 0;
    uint8_t plane_count = 0;
    uint32_t color_range = 0;
};

class Collector {
public:
    void push(FrameInfo f) {
        std::lock_guard<std::mutex> l(mu_);
        msgs_.push_back(std::move(f));
    }
    size_t size() {
        std::lock_guard<std::mutex> l(mu_);
        return msgs_.size();
    }
    std::vector<FrameInfo> snapshot() {
        std::lock_guard<std::mutex> l(mu_);
        return msgs_;
    }

private:
    std::mutex mu_;
    std::vector<FrameInfo> msgs_;
};

// 从一帧采集 FrameInfo（CPU 帧的载荷 + 元数据）
FrameInfo info_of(const KopawFrame* frame) {
    FrameInfo f;
    f.pts = frame->pts;
    f.dts = frame->dts;
    f.width = frame->format.video.width;
    f.height = frame->format.video.height;
    f.stride = frame->stride;
    f.memory_type = frame->memory_type;
    f.plane_count = frame->plane_count;
    f.color_range = frame->color.range;
    const uint8_t* data = kopaw::cpu_data(frame);
    if (data && frame->size > 0) {
        f.payload.assign(data, data + frame->size);
    }
    return f;
}

class CollectorNode {
public:
    explicit CollectorNode(Collector* col) : col_(col) {}

    KopawNodeDesc desc() {
        KopawNodeDesc d{};
        d.struct_size = sizeof(d);
        d.name = "collector";
        d.user_data = this;
        d.outputs = 0;
        d.inputs = 1;
        d.queue_capacity = 32;
        d.is_sink = 1;
        d.self_driven = 0;
        d.vtable = &kVTable;
        return d;
    }
    void set_graph(KopawGraph* g, uint32_t id) {
        graph_ = g;
        node_id_ = id;
    }
    int32_t send_impl(KopawFrame* frame) {
        if (!frame) return KOPAW_E_INVALID;
        if (frame->flags & KOPAW_FRAME_FLAG_EOS) {
            if (frame->release) frame->release(frame);
            kopaw_node_sink_done(graph_, node_id_);
            return KOPAW_OK;
        }
        col_->push(info_of(frame));
        if (frame->release) frame->release(frame);
        return KOPAW_OK;
    }

private:
    static const KopawNodeVTable kVTable;
    Collector* col_;
    KopawGraph* graph_ = nullptr;
    uint32_t node_id_ = 0;
};

const KopawNodeVTable CollectorNode::kVTable = [] {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = [](void* user, KopawFrame* frame) -> int32_t {
        return static_cast<CollectorNode*>(user)->send_impl(frame);
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<CollectorNode*>(user); };
    return vt;
}();

// 测试用源：依次 emit N 个固定载荷后发 EOS（元数据随帧携带，验证信封头部）
class PacketSourceNode {
public:
    explicit PacketSourceNode(std::vector<std::string> payloads)
        : payloads_(std::move(payloads)) {}

    KopawNodeDesc desc(KopawGraph* g) {
        graph_ = g;
        KopawNodeDesc d{};
        d.struct_size = sizeof(d);
        d.name = "packet-source";
        d.user_data = this;
        d.outputs = 1;
        d.inputs = 0;
        d.queue_capacity = 32;
        d.is_sink = 0;
        d.self_driven = 1;
        d.vtable = &kVTable;
        return d;
    }
    void set_output(KopawOutput out) { out_ = out; }
    int32_t run_impl() {
        for (size_t i = 0; i < payloads_.size(); ++i) {
            kopaw::OwnedFrame* o =
                kopaw::make_frame(KOPAW_MEDIA_VIDEO, static_cast<int64_t>(i * 1000 + 7),
                                  static_cast<int64_t>(i * 500 + 3), payloads_[i].size());
            std::memcpy(o->data(), payloads_[i].data(), payloads_[i].size());
            o->frame.format.video.width = 320 + static_cast<uint32_t>(i);
            o->frame.format.video.height = 240;
            o->frame.stride = (320 + static_cast<uint32_t>(i)) * 4;
            o->frame.color.range = KOPAW_COLOR_RANGE_FULL;
            o->frame.color.matrix = KOPAW_COLOR_MATRIX_BT709;
            if (kopaw_graph_emit(graph_, out_, o->ptr()) != KOPAW_OK) return KOPAW_E_STOPPED;
        }
        kopaw::OwnedFrame* eos = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0);
        eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(graph_, out_, eos->ptr());
        return KOPAW_OK;
    }

private:
    static const KopawNodeVTable kVTable;
    std::vector<std::string> payloads_;
    KopawGraph* graph_ = nullptr;
    KopawOutput out_{};
};

const KopawNodeVTable PacketSourceNode::kVTable = [] {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.run = [](void* user) -> int32_t {
        return static_cast<PacketSourceNode*>(user)->run_impl();
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<PacketSourceNode*>(user); };
    vt.bind_output = [](void* user, uint32_t port, KopawOutput output) {
        if (port == 0) static_cast<PacketSourceNode*>(user)->set_output(output);
    };
    return vt;
}();

// DMA-BUF 测试源：emit 一个双平面 memfd 帧并携带 acquire fence fd，随后 EOS。
// fd 所有权在 emit 后移交图（帧 release 负责关闭），故析构只在未 emit 时回收。
class DmabufSourceNode {
public:
    struct Plane {
        int fd = -1;
        uint32_t stride = 0;
    };

    DmabufSourceNode(Plane p0, Plane p1, int fence_fd, uint32_t w, uint32_t h, int64_t pts)
        : p0_(p0), p1_(p1), fence_fd_(fence_fd), w_(w), h_(h), pts_(pts) {}
    ~DmabufSourceNode() {
        if (!emitted_) {
            if (p0_.fd >= 0) ::close(p0_.fd);
            if (p1_.fd >= 0) ::close(p1_.fd);
            if (fence_fd_ >= 0) ::close(fence_fd_);
        }
    }

    KopawNodeDesc desc(KopawGraph* g) {
        graph_ = g;
        KopawNodeDesc d{};
        d.struct_size = sizeof(d);
        d.name = "dmabuf-source";
        d.user_data = this;
        d.outputs = 1;
        d.inputs = 0;
        d.queue_capacity = 4;
        d.is_sink = 0;
        d.self_driven = 1;
        d.vtable = &kVTable;
        return d;
    }
    void set_output(KopawOutput out) { out_ = out; }
    int32_t run_impl() {
        kopaw::OwnedFrame* o = kopaw::make_external_frame(KOPAW_MEDIA_VIDEO, pts_, 0);
        o->frame.drm_fourcc = kopaw::kDrmFormatNv12;
        o->frame.format.video.width = w_;
        o->frame.format.video.height = h_;
        o->frame.stride = w_;
        o->frame.plane_count = 2;
        o->frame.planes[0] = {p0_.fd, 0u, p0_.stride, kopaw::kDrmFormatModLinear};
        o->frame.planes[1] = {p1_.fd, 0u, p1_.stride, kopaw::kDrmFormatModLinear};
        o->frame.acquire_fence.kind = KOPAW_SYNC_FENCE_FD;
        o->frame.acquire_fence.fd = fence_fd_;
        emitted_ = true;  // 之后 fd 属于帧
        if (kopaw_graph_emit(graph_, out_, o->ptr()) != KOPAW_OK) return KOPAW_E_STOPPED;
        kopaw::OwnedFrame* eos = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0);
        eos->frame.flags |= KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(graph_, out_, eos->ptr());
        return KOPAW_OK;
    }

private:
    static const KopawNodeVTable kVTable;
    Plane p0_;
    Plane p1_;
    int fence_fd_ = -1;
    uint32_t w_ = 0;
    uint32_t h_ = 0;
    int64_t pts_ = 0;
    bool emitted_ = false;
    KopawGraph* graph_ = nullptr;
    KopawOutput out_{};
};

const KopawNodeVTable DmabufSourceNode::kVTable = [] {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.run = [](void* user) -> int32_t {
        return static_cast<DmabufSourceNode*>(user)->run_impl();
    };
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<DmabufSourceNode*>(user); };
    vt.bind_output = [](void* user, uint32_t port, KopawOutput output) {
        if (port == 0) static_cast<DmabufSourceNode*>(user)->set_output(output);
    };
    return vt;
}();

// ---- 通用工具 ----

bool bind_server(kopnet::TunnelServer& server, std::string& uri, int port_base,
                 std::string* error) {
    for (int port = port_base; port < port_base + 60; ++port) {
        uri = "tcp://127.0.0.1:" + std::to_string(port);
        if (server.listen(uri, error)) return true;
    }
    return false;
}

bool wait_until(std::function<bool()> pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// 把一段 CPU 载荷序列化成帧信封（服务端下行用）
std::vector<uint8_t> build_cpu_envelope(const std::string& payload, int64_t pts, uint32_t w,
                                        uint32_t h) {
    std::unique_ptr<kopaw::OwnedFrame> o(
        kopaw::make_frame(KOPAW_MEDIA_VIDEO, pts, pts / 2, payload.size()));
    std::memcpy(o->data(), payload.data(), payload.size());
    o->frame.format.video.width = w;
    o->frame.format.video.height = h;
    o->frame.stride = w * 4;
    o->frame.color.range = KOPAW_COLOR_RANGE_LIMITED;
    o->frame.color.matrix = KOPAW_COLOR_MATRIX_BT601;
    std::vector<uint8_t> wire;
    uint32_t plane_fds = 0;
    bool fence = false;
    std::string err;
    if (!kopnet::serialize_frame(o->frame, &wire, &plane_fds, &fence, &err)) {
        std::fprintf(stderr, "FAIL serialize_frame: %s\n", err.c_str());
        ++g_failures;
        return {};
    }
    return wire;
}

// 解析一条帧信封（服务端上行用），返回 FrameInfo；失败置 failures
bool parse_envelope(const std::vector<uint8_t>& wire, const std::vector<int>& fds,
                    FrameInfo* out) {
    std::unique_ptr<kopaw::OwnedFrame> o(kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0));
    std::vector<uint8_t> payload;
    std::string err;
    if (!kopnet::deserialize_frame(wire.data(), wire.size(), fds.data(),
                                   fds.size(), o->ptr(), &payload, &err)) {
        std::fprintf(stderr, "FAIL deserialize_frame: %s\n", err.c_str());
        ++g_failures;
        return false;
    }
    *out = info_of(o->ptr());
    out->payload = std::move(payload);
    return true;
}

bool write_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        const ssize_t n = ::write(fd, p, len);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// 创建一个 memfd 并写入指定内容（DMA-BUF 平面 / fence 的替身）
int create_memfd(const void* data, size_t len) {
    const int fd = static_cast<int>(::syscall(__NR_memfd_create, "kopnet-test", 0u));
    if (fd < 0) return -1;
    if (!write_all(fd, data, len)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool fd_bytes_equal(int fd, uint64_t offset, const uint8_t* expect, size_t len) {
    if (fd < 0 || len == 0) return false;
    void* map = ::mmap(nullptr, static_cast<size_t>(offset) + len, PROT_READ,
                       MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return false;
    const bool ok = std::memcmp(static_cast<const char*>(map) + offset, expect, len) == 0;
    ::munmap(map, static_cast<size_t>(offset) + len);
    return ok;
}

bool fd_pread_equals(int fd, const std::string& expect) {
    if (fd < 0) return false;
    char buf[64];
    const ssize_t n = ::pread(fd, buf, expect.size(), 0);
    return n == static_cast<ssize_t>(expect.size()) &&
           std::memcmp(buf, expect.data(), static_cast<size_t>(n)) == 0;
}

// ---- 1) 下行：TCP 隧道 + CPU 帧信封 ----

void test_source_node() {
    kopnet::TunnelServer server;
    std::string uri;
    std::string error;
    CHECK(bind_server(server, uri, 19000, &error), error.c_str());
    std::vector<std::unique_ptr<kopnet::TunnelSession>> held;
    std::mutex held_mu;
    std::vector<std::vector<uint8_t>> envelopes;
    std::vector<std::string> payloads;
    const int kN = 8;
    for (int i = 0; i < kN; ++i) {
        const std::string p = "stream-packet-" + std::to_string(i);
        payloads.push_back(p);
        envelopes.push_back(build_cpu_envelope(p, i * 1000 + 7, 200 + i, 120));
    }

    std::thread server_thread([&] {
        server.run([&](std::unique_ptr<kopnet::TunnelSession> session) {
            kopnet::TunnelSession* raw = session.get();
            {
                std::lock_guard<std::mutex> l(held_mu);
                held.push_back(std::move(session));
            }
            raw->set_channel_handler([&](uint32_t id, const std::string& kind,
                                         kopnet::ChannelMode) {
                if (kind != "media") return;
                std::thread([&, id] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    for (const auto& env : envelopes) {
                        raw->send(id, env.data(), env.size(), nullptr, 3000);
                    }
                }).detach();
            });
        });
    });

    Collector collected;
    KopawGraph* g = kopaw_graph_new();
    CHECK(g != nullptr, "创建图失败");
    auto* src = new kopaw::NetSourceNode(
        kopaw::NetSourceNode::Options{uri, "media", kopnet::ChannelMode::Stream, 20000, 32});
    CHECK(src->open(&error), error.c_str());
    KopawNodeDesc src_desc = src->desc(g);
    uint32_t src_id = kopaw_graph_add_node(g, &src_desc);
    CHECK(src_id != 0, "加入源节点失败");
    src->set_output(kopaw_graph_node_output(g, src_id, 0));
    auto* sink = new CollectorNode(&collected);
    KopawNodeDesc sink_desc = sink->desc();
    uint32_t sink_id = kopaw_graph_add_node(g, &sink_desc);
    CHECK(sink_id != 0, "加入采集节点失败");
    CHECK(kopaw_graph_connect(g, kopaw_graph_node_output(g, src_id, 0), sink_id, 0, 16) ==
              KOPAW_OK,
          "连线失败");
    CHECK(kopaw_graph_start(g) == KOPAW_OK, "图启动失败");

    CHECK(wait_until([&] { return collected.size() >= kN; }, 15000), "未收齐报文");
    auto got = collected.snapshot();
    CHECK(got.size() == payloads.size(), "报文数量不一致");
    for (size_t i = 0; i < got.size() && i < payloads.size(); ++i) {
        const std::vector<uint8_t> expect(payloads[i].begin(), payloads[i].end());
        CHECK(got[i].payload == expect, "报文内容不一致");
        CHECK(got[i].pts == static_cast<int64_t>(i * 1000 + 7), "pts 未透传");
        CHECK(got[i].dts == static_cast<int64_t>(i * 500 + 3), "dts 未透传");
        CHECK(got[i].width == 200u + i, "width 未透传");
        CHECK(got[i].height == 120u, "height 未透传");
        CHECK(got[i].stride == (200u + i) * 4u, "stride 未透传");
        CHECK(got[i].memory_type == KOPAW_MEMORY_CPU, "memory_type 应为 CPU");
        CHECK(got[i].color_range == KOPAW_COLOR_RANGE_LIMITED, "color.range 未透传");
    }

    kopaw_graph_stop(g, 3000);
    kopaw_graph_free(g);
    {
        std::lock_guard<std::mutex> l(held_mu);
        for (auto& s : held) s->stop();
    }
    server.stop();
    server_thread.join();
}

// ---- 2) 上行：TCP 隧道 + CPU 帧信封 ----

void test_sink_node() {
    kopnet::TunnelServer server;
    std::string uri;
    std::string error;
    CHECK(bind_server(server, uri, 19100, &error), error.c_str());
    std::vector<std::unique_ptr<kopnet::TunnelSession>> held;
    std::mutex held_mu;
    Collector collected;
    std::atomic<int> received{0};

    std::thread server_thread([&] {
        server.run([&](std::unique_ptr<kopnet::TunnelSession> session) {
            kopnet::TunnelSession* raw = session.get();
            {
                std::lock_guard<std::mutex> l(held_mu);
                held.push_back(std::move(session));
            }
            raw->set_channel_handler([&](uint32_t id, const std::string& kind,
                                         kopnet::ChannelMode) {
                if (kind != "media") return;
                std::thread([&, id] {
                    while (raw->alive()) {
                        std::vector<uint8_t> data;
                        std::vector<int> fds;
                        if (raw->recv(id, &data, &fds, 500) !=
                            kopnet::RecvStatus::Ok)
                            continue;
                        FrameInfo info;
                        if (parse_envelope(data, fds, &info)) {
                            collected.push(std::move(info));
                            received.fetch_add(1);
                        }
                    }
                }).detach();
            });
        });
    });

    std::vector<std::string> payloads;
    const int kN = 6;
    for (int i = 0; i < kN; ++i) payloads.push_back("uplink-" + std::to_string(i));

    KopawGraph* g = kopaw_graph_new();
    CHECK(g != nullptr, "创建图失败");
    auto* psrc = new PacketSourceNode(payloads);
    KopawNodeDesc psrc_desc = psrc->desc(g);
    uint32_t psrc_id = kopaw_graph_add_node(g, &psrc_desc);
    CHECK(psrc_id != 0, "加入测试源失败");
    psrc->set_output(kopaw_graph_node_output(g, psrc_id, 0));
    auto* nsink = new kopaw::NetSinkNode(
        kopaw::NetSinkNode::Options{uri, "media", kopnet::ChannelMode::Stream, 5000, 32});
    CHECK(nsink->open(&error), error.c_str());
    KopawNodeDesc nsink_desc = nsink->desc();
    uint32_t nsink_id = kopaw_graph_add_node(g, &nsink_desc);
    CHECK(nsink_id != 0, "加入网络 sink 失败");
    CHECK(kopaw_graph_connect(g, kopaw_graph_node_output(g, psrc_id, 0), nsink_id, 0, 16) ==
              KOPAW_OK,
          "连线失败");
    CHECK(kopaw_graph_start(g) == KOPAW_OK, "图启动失败");

    CHECK(wait_until([&] { return received.load() >= kN; }, 15000), "服务端未收齐报文");
    auto got = collected.snapshot();
    CHECK(got.size() == payloads.size(), "上行报文数量不一致");
    for (size_t i = 0; i < got.size() && i < payloads.size(); ++i) {
        const std::vector<uint8_t> expect(payloads[i].begin(), payloads[i].end());
        CHECK(got[i].payload == expect, "上行报文内容不一致");
        CHECK(got[i].pts == static_cast<int64_t>(i * 1000 + 7), "上行 pts 未透传");
        CHECK(got[i].width == 320u + i, "上行 width 未透传");
        CHECK(got[i].memory_type == KOPAW_MEMORY_CPU, "上行 memory_type 应为 CPU");
        CHECK(got[i].color_range == KOPAW_COLOR_RANGE_FULL, "上行 color.range 未透传");
    }

    kopaw_graph_stop(g, 3000);
    kopaw_graph_free(g);
    {
        std::lock_guard<std::mutex> l(held_mu);
        for (auto& s : held) s->stop();
    }
    server.stop();
    server_thread.join();
}

// ---- 3) DMA-BUF 透传：Unix 隧道 + SCM_RIGHTS ----

void test_dmabuf_passthrough() {
    const std::string sock =
        "/tmp/kopnet-node-dmabuf-" + std::to_string(::getpid()) + ".sock";
    ::unlink(sock.c_str());
    const std::string uri = "unix:" + sock;

    // 两平面内容（等长模式，便于整段比对）+ fence 哨兵
    std::vector<uint8_t> plane0(2048);
    std::vector<uint8_t> plane1(1024);
    for (size_t i = 0; i < plane0.size(); ++i) plane0[i] = static_cast<uint8_t>(i & 0xFFu);
    for (size_t i = 0; i < plane1.size(); ++i)
        plane1[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    const std::string fence_sentinel = "KOPNET-FENCE";

    const int fd0 = create_memfd(plane0.data(), plane0.size());
    const int fd1 = create_memfd(plane1.data(), plane1.size());
    const int fence_fd = create_memfd(fence_sentinel.data(), fence_sentinel.size());
    CHECK(fd0 >= 0 && fd1 >= 0 && fence_fd >= 0, "memfd 创建失败");
    if (fd0 < 0 || fd1 < 0 || fence_fd < 0) return;

    const uint32_t kW = 64;
    const uint32_t kH = 32;
    const int64_t kPts = 1234567;

    kopnet::TunnelServer server;
    std::string error;
    CHECK(server.listen(uri, &error), error.c_str());
    std::vector<std::unique_ptr<kopnet::TunnelSession>> held;
    std::mutex held_mu;
    std::atomic<bool> verified{false};

    std::thread server_thread([&] {
        server.run([&](std::unique_ptr<kopnet::TunnelSession> session) {
            kopnet::TunnelSession* raw = session.get();
            {
                std::lock_guard<std::mutex> l(held_mu);
                held.push_back(std::move(session));
            }
            raw->set_channel_handler([&](uint32_t id, const std::string& kind,
                                         kopnet::ChannelMode) {
                if (kind != "media") return;
                std::thread([&, id] {
                    while (raw->alive() && !verified.load()) {
                        std::vector<uint8_t> data;
                        std::vector<int> fds;
                        if (raw->recv(id, &data, &fds, 500) !=
                            kopnet::RecvStatus::Ok)
                            continue;
                        if (data.empty()) continue;
                        // 拿到 fd 就必须由帧的 release 关闭
                        std::unique_ptr<kopaw::OwnedFrame> o(
                            kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0));
                        std::vector<uint8_t> payload;
                        std::string err;
                        if (!kopnet::deserialize_frame(data.data(), data.size(), fds.data(),
                                                       fds.size(), o->ptr(), &payload, &err)) {
                            std::fprintf(stderr, "FAIL dmabuf deserialize: %s\n", err.c_str());
                            ++g_failures;
                            return;
                        }
                        CHECK(o->frame.memory_type == KOPAW_MEMORY_DMABUF,
                              "memory_type 不是 DMABUF");
                        CHECK(o->frame.plane_count == 2, "plane_count 应为 2");
                        CHECK(o->frame.drm_fourcc == kopaw::kDrmFormatNv12, "drm_fourcc 丢失");
                        CHECK(o->frame.format.video.width == kW, "width 丢失");
                        CHECK(o->frame.format.video.height == kH, "height 丢失");
                        CHECK(o->frame.pts == kPts, "pts 丢失");
                        CHECK(o->frame.planes[0].stride == kW, "plane0 stride 丢失");
                        CHECK(o->frame.planes[1].stride == kW / 2, "plane1 stride 丢失");
                        CHECK(o->frame.planes[0].modifier == kopaw::kDrmFormatModLinear,
                              "modifier 丢失");
                        // 平面内容必须与各自的 fd 对齐（错位会立刻失败）
                        CHECK(fd_bytes_equal(o->frame.planes[0].fd, o->frame.planes[0].offset,
                                             plane0.data(), plane0.size()),
                              "plane0 内容不一致");
                        CHECK(fd_bytes_equal(o->frame.planes[1].fd, o->frame.planes[1].offset,
                                             plane1.data(), plane1.size()),
                              "plane1 内容不一致");
                        // fence fd 应在平面 fd 之后到达，且可读回哨兵
                        CHECK(o->frame.acquire_fence.kind == KOPAW_SYNC_FENCE_FD,
                              "fence kind 丢失");
                        CHECK(fd_pread_equals(o->frame.acquire_fence.fd, fence_sentinel),
                              "fence fd 内容不一致");
                        verified.store(true);
                    }
                }).detach();
            });
        });
    });

    KopawGraph* g = kopaw_graph_new();
    CHECK(g != nullptr, "创建图失败");
    auto* dsrc = new DmabufSourceNode(DmabufSourceNode::Plane{fd0, kW},
                                      DmabufSourceNode::Plane{fd1, kW / 2}, fence_fd, kW, kH,
                                      kPts);
    KopawNodeDesc dsrc_desc = dsrc->desc(g);
    uint32_t dsrc_id = kopaw_graph_add_node(g, &dsrc_desc);
    CHECK(dsrc_id != 0, "加入 DMA-BUF 源失败");
    dsrc->set_output(kopaw_graph_node_output(g, dsrc_id, 0));
    auto* nsink = new kopaw::NetSinkNode(
        kopaw::NetSinkNode::Options{uri, "media", kopnet::ChannelMode::Stream, 5000, 4});
    CHECK(nsink->open(&error), error.c_str());
    KopawNodeDesc nsink_desc = nsink->desc();
    uint32_t nsink_id = kopaw_graph_add_node(g, &nsink_desc);
    CHECK(nsink_id != 0, "加入网络 sink 失败");
    CHECK(kopaw_graph_connect(g, kopaw_graph_node_output(g, dsrc_id, 0), nsink_id, 0, 4) ==
              KOPAW_OK,
          "连线失败");
    CHECK(kopaw_graph_start(g) == KOPAW_OK, "图启动失败");

    CHECK(wait_until([&] { return verified.load(); }, 15000), "DMA-BUF 帧未到达服务端");

    kopaw_graph_stop(g, 3000);
    kopaw_graph_free(g);
    {
        std::lock_guard<std::mutex> l(held_mu);
        for (auto& s : held) s->stop();
    }
    server.stop();
    server_thread.join();
    ::unlink(sock.c_str());
}

}  // namespace

// ---- 4) 断线自动重连：服务端杀掉后在原端口重启，帧继续流动 ----

class FrameServer {
public:
    std::string uri;
    std::atomic<int> sessions{0};

    bool start(int port_base, const std::vector<std::vector<uint8_t>>& envelopes,
               std::string* error) {
        if (!bind_server(server_, uri, port_base, error)) return false;
        envelopes_ = &envelopes;
        thread_ = std::thread([this] {
            server_.run([&](std::unique_ptr<kopnet::TunnelSession> session) {
                kopnet::TunnelSession* raw = session.get();
                {
                    std::lock_guard<std::mutex> l(mu_);
                    held_.push_back(std::move(session));
                }
                sessions.fetch_add(1);
                raw->set_channel_handler(
                    [this, raw](uint32_t id, const std::string& kind, kopnet::ChannelMode) {
                        if (kind != "media") return;
                        std::thread([this, raw, id] {
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                            for (const auto& env : *envelopes_) {
                                raw->send(id, env.data(), env.size(), nullptr, 3000);
                            }
                        }).detach();
                    });
            });
        });
        return true;
    }

    void stop() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> l(mu_);
        for (auto& s : held_) s->stop();
        held_.clear();
    }

    ~FrameServer() { stop(); }

private:
    kopnet::TunnelServer server_;
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::unique_ptr<kopnet::TunnelSession>> held_;
    const std::vector<std::vector<uint8_t>>* envelopes_ = nullptr;
};

void test_source_reconnect() {
    std::string error;
    const std::string payload = "reconnect-packet";
    const int kBatch = 4;
    std::vector<std::vector<uint8_t>> envelopes;
    for (int i = 0; i < kBatch; ++i) {
        envelopes.push_back(build_cpu_envelope(payload, i * 100, 64, 48));
    }

    // 主服务端 A + 备用服务端 B：杀掉 A 后客户端应故障转移到 B
    FrameServer server_a;
    CHECK(server_a.start(19300, envelopes, &error), error.c_str());

    Collector collected;
    KopawGraph* g = kopaw_graph_new();
    CHECK(g != nullptr, "创建图失败");
    kopaw::NetSourceNode::Options opts;
    opts.endpoint = server_a.uri;
    opts.fallback_endpoints = {"tcp://127.0.0.1:19400"};
    opts.kind = "media";
    opts.mode = kopnet::ChannelMode::Stream;
    opts.idle_timeout_ms = 30000;  // 断连期间给重连留足时间
    opts.queue_capacity = 32;
    opts.auto_reconnect = true;
    opts.reconnect_base_delay_ms = 200;
    opts.reconnect_max_delay_ms = 1000;
    auto* src = new kopaw::NetSourceNode(opts);
    CHECK(src->open(&error), error.c_str());
    KopawNodeDesc src_desc = src->desc(g);
    uint32_t src_id = kopaw_graph_add_node(g, &src_desc);
    CHECK(src_id != 0, "加入源节点失败");
    src->set_output(kopaw_graph_node_output(g, src_id, 0));
    auto* sink = new CollectorNode(&collected);
    KopawNodeDesc sink_desc = sink->desc();
    uint32_t sink_id = kopaw_graph_add_node(g, &sink_desc);
    CHECK(sink_id != 0, "加入采集节点失败");
    CHECK(kopaw_graph_connect(g, kopaw_graph_node_output(g, src_id, 0), sink_id, 0, 16) ==
              KOPAW_OK,
          "连线失败");
    CHECK(kopaw_graph_start(g) == KOPAW_OK, "图启动失败");

    CHECK(wait_until([&] { return collected.size() >= kBatch; }, 15000), "首批未收齐");
    CHECK(server_a.sessions.load() == 1, "首批应只接入一次");

    // 杀掉主服务端，起备用服务端：客户端应自动故障转移过去
    server_a.stop();
    FrameServer server_b;
    CHECK(server_b.start(19400, envelopes, &error), error.c_str());
    CHECK(wait_until([&] { return server_b.sessions.load() >= 1; }, 15000),
          "客户端未故障转移到备用端点");
    CHECK(wait_until([&] { return collected.size() >= 2 * kBatch; }, 15000),
          "重连后第二批未收齐");

    kopaw_graph_stop(g, 3000);
    kopaw_graph_free(g);
    server_b.stop();
}

int main() {
    test_source_node();
    test_sink_node();
    test_dmabuf_passthrough();
    test_source_reconnect();
    if (g_failures == 0) {
        std::printf("kopnet-node-test: ALL PASS\n");
        return 0;
    }
    std::printf("kopnet-node-test: %d FAILURES\n", g_failures);
    return 1;
}
