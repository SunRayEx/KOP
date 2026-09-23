// KOPNET 远程会话测试：KOPMS 线协议跑在隧道通道上的端到端回路。
//
// serve 侧手工 accept 一条 unix 传输（不用 TunnelServer::run：它会先 start
// 隧道再回调，通道通知存在竞态），交给 RemoteSession::serve；connect 侧用
// RemoteSession::open 直拨。DMA-BUF 平面用 memfd 伪装，验证 fd 随
// FRAME_SUBMIT 透传并在对端可回读。
//
// 监听器一律在主线程先 bind 好，再起接受线程，避免“客户端先于 bind 拨号”
// 的 ENOENT 竞态。
#include "kopnet/remote_session.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kopnet/adapters.hpp"
#include "kopnet/endpoint.hpp"
#include "kopnet/transport_listener.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "! %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using kopnet::FrameDisposition;
using kopnet::IoStatus;
using kopnet::RemoteFrame;
using kopnet::RemoteSession;
using kopnet::TunnelSession;

constexpr const char* kPixelMagic = "kopnet-remote-dmabuf-payload";
constexpr size_t kPixelSize = 4096;

void noop_retain(KopmsFrameDescriptor* /*frame*/) {}
void noop_release(KopmsFrameDescriptor* /*frame*/) {}

int make_memfd(const void* payload, size_t len) {
    const int fd = memfd_create("kopnet-remote-test", 0);
    CHECK(fd >= 0, "memfd_create 失败");
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(kPixelSize)) != 0) {
        CHECK(false, "ftruncate 失败");
        ::close(fd);
        return -1;
    }
    void* map = mmap(nullptr, kPixelSize, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        CHECK(false, "mmap 失败");
        ::close(fd);
        return -1;
    }
    std::memcpy(map, payload, len);
    munmap(map, kPixelSize);
    return fd;
}

bool read_fd_fully(int fd, void* out, size_t len) {
    char* dst = static_cast<char*>(out);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = pread(fd, dst + done, len - done, 0);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

KopmsFrameDescriptor make_descriptor(int fd) {
    KopmsFrameDescriptor frame{};
    frame.struct_size = sizeof(frame);
    frame.version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    frame.media_type = KOPAW_MEDIA_VIDEO;
    frame.width = 64;
    frame.height = 64;
    frame.format = 0x3231564e;  // DRM_FORMAT_NV12（仅用于线协议字段携带）
    frame.memory_type = KOPAW_MEMORY_DMABUF;
    frame.plane_count = 1;
    frame.planes[0].fd = fd;
    frame.planes[0].offset = 0;
    frame.planes[0].stride = 64;
    frame.planes[0].modifier = 0;  // DRM_FORMAT_MOD_LINEAR
    frame.acquire_fence.kind = KOPAW_SYNC_FENCE_NONE;
    frame.dma_buf_handle = static_cast<uint64_t>(static_cast<uint32_t>(fd));
    frame.retain = noop_retain;
    frame.release = noop_release;
    return frame;
}

std::unique_ptr<kopnet::TransportListener> bind_unix(const std::string& path,
                                                     std::string* error) {
    std::unique_ptr<kopnet::TransportListener> listener;
    if (!kopnet::AdapterRegistry::instance().serve("unix://" + path, &listener,
                                                   error)) {
        return nullptr;
    }
    return listener;
}

// 绑定一个空闲的 TCP 监听器；port 为起始端口，成功时写出实际端口
std::unique_ptr<kopnet::TransportListener> bind_tcp(int* port, std::string* error) {
    for (int p = *port; p < *port + 32; ++p) {
        std::unique_ptr<kopnet::TransportListener> listener;
        const std::string uri = "tcp://127.0.0.1:" + std::to_string(p);
        if (kopnet::AdapterRegistry::instance().serve(uri, &listener, error)) {
            *port = p;
            return listener;
        }
    }
    return nullptr;
}

// 阻塞接受一条已绑定监听器上的连接，装回调后 start 隧道。
std::unique_ptr<RemoteSession>
accept_and_serve(kopnet::TransportListener* listener, std::string* error) {
    std::unique_ptr<kopnet::Transport> transport;
    if (listener->accept(&transport, 5000, error) != IoStatus::Ok)
        return nullptr;
    auto tunnel =
        TunnelSession::create(std::move(transport), /*is_dialer=*/false,
                              TunnelSession::Options(), error);
    if (!tunnel) return nullptr;
    auto session = std::make_unique<RemoteSession>();
    if (!session->serve(std::move(tunnel), RemoteSession::Options(), error))
        return nullptr;
    return session;
}

// 连接 + HELLO 的公共前置步骤。
bool dial_and_hello(RemoteSession* client, const std::string& path,
                    uint64_t capabilities, std::string* error) {
    RemoteSession::Options opts;
    opts.hello_timeout_ms = 5000;
    if (!client->open("unix://" + path, opts, error)) return false;
    return client->hello(capabilities, error);
}

bool test_handshake_and_ping() {
    const std::string path = "/tmp/kopnet-remote-handshake.sock";
    ::unlink(path.c_str());
    std::string error;
    std::string bind_error;
    auto listener = bind_unix(path, &bind_error);
    CHECK(listener != nullptr, bind_error.c_str());
    if (!listener) return false;

    std::mutex server_mutex;
    std::unique_ptr<RemoteSession> server_session;
    std::thread server_thread([&] {
        std::string server_error;
        auto session = accept_and_serve(listener.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        std::lock_guard<std::mutex> lock(server_mutex);
        server_session = std::move(session);
        if (server_session) server_session->wait_done(8000);
    });

    const uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF |
                                 KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                                 KOPMS_PROTOCOL_CAP_CONTROL_STATE |
                                 KOPMS_PROTOCOL_CAP_COLOR_METADATA;
    RemoteSession client;
    CHECK(dial_and_hello(&client, path, kClientCaps, &error), error.c_str());
    CHECK(client.connected(), "HELLO 后 connected 应为真");
    CHECK((client.negotiated_capabilities() & KOPMS_PROTOCOL_CAP_DMABUF) != 0,
          "协商结果应包含 DMABUF");
    CHECK(client.negotiated_minor() <= KOPMS_PROTOCOL_MINOR,
          "协商 minor 不应超过本端版本");
    CHECK(client.ping(&error), error.c_str());

    client.disconnect();
    server_thread.join();
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        CHECK(server_session != nullptr, "服务端会话未建立");
        if (server_session)
            CHECK(server_session->wait_done(5000), "GOODBYE 后应 done");
    }
    return g_failures == 0;
}

bool test_frame_roundtrip_with_fds() {
    const std::string path = "/tmp/kopnet-remote-frame.sock";
    ::unlink(path.c_str());
    std::string error;
    std::string bind_error;
    auto listener = bind_unix(path, &bind_error);
    CHECK(listener != nullptr, bind_error.c_str());
    if (!listener) return false;

    std::mutex server_mutex;
    // 所有权由两端共享：服务端线程 wait_done 期间不持锁，主线程 release_frame
    // 期间会话也保持存活。
    std::shared_ptr<RemoteSession> server_session;
    std::promise<uint32_t> frame_arrived;
    std::future<uint32_t> arrived = frame_arrived.get_future();
    RemoteFrame retained;
    std::vector<uint8_t> received_pixels(kPixelSize);

    std::thread server_thread([&] {
        std::string server_error;
        auto session = accept_and_serve(listener.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        if (!session) return;
        session->set_frame_handler([&](RemoteFrame frame) -> FrameDisposition {
            const bool ok = frame.fds.size() == 1 &&
                            read_fd_fully(frame.fds[0], received_pixels.data(),
                                          std::strlen(kPixelMagic));
            CHECK(ok, "服务端未回读 memfd 内容");
            const uint32_t id = frame.payload.frame_id;
            // Retain：std::move 取走帧所有权，稍后由主线程显式回送 release。
            retained = std::move(frame);
            frame_arrived.set_value(id);
            return FrameDisposition::Retain;
        });
        auto shared = std::shared_ptr<RemoteSession>(session.release());
        {
            std::lock_guard<std::mutex> lock(server_mutex);
            server_session = shared;
        }
        // 等待期间不持 server_mutex，否则主线程的 release_frame 会被阻塞。
        shared->wait_done(8000);
    });

    const uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF |
                                 KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                                 KOPMS_PROTOCOL_CAP_COLOR_METADATA;
    RemoteSession client;
    CHECK(dial_and_hello(&client, path, kClientCaps, &error), error.c_str());

    const int fd = make_memfd(kPixelMagic, std::strlen(kPixelMagic));
    CHECK(fd >= 0, "构造 memfd 失败");
    KopmsFrameDescriptor frame = make_descriptor(fd);
    uint32_t frame_id = 0;
    CHECK(client.submit(&frame, &frame_id, &error), error.c_str());
    CHECK(frame_id != 0, "submit 应返回非零 frame_id");

    // 服务端先收到帧（Retain 期间尚不回送 release）。
    const bool arrived_ok = arrived.wait_for(std::chrono::seconds(5)) ==
                            std::future_status::ready;
    CHECK(arrived_ok, "服务端未在超时内收到 FRAME_SUBMIT");
    const uint32_t server_frame_id = arrived.get();
    CHECK(server_frame_id == frame_id, "两端 frame_id 应一致");
    const size_t magic_len = std::strlen(kPixelMagic);
    CHECK(std::memcmp(received_pixels.data(), kPixelMagic, magic_len) == 0,
          "透传的 memfd 内容不一致");

    std::shared_ptr<RemoteSession> server;
    {
        std::lock_guard<std::mutex> lock(server_mutex);
        server = server_session;
    }
    CHECK(server != nullptr, "服务端会话未建立");
    if (server) {
        CHECK(server->release_frame(server_frame_id, KOPMS_FRAME_RELEASE_OK,
                                     &error),
              error.c_str());
    }
    CHECK(client.wait_for_release(frame_id, 5000, &error), error.c_str());
    retained.close_fds();  // 应用侧显式关闭保留的 fd

    client.disconnect();
    server_thread.join();
    ::close(fd);
    return g_failures == 0;
}

bool test_control_request_and_ack() {
    const std::string path = "/tmp/kopnet-remote-control.sock";
    ::unlink(path.c_str());
    std::string error;
    std::string bind_error;
    auto listener = bind_unix(path, &bind_error);
    CHECK(listener != nullptr, bind_error.c_str());
    if (!listener) return false;

    std::mutex server_mutex;
    std::unique_ptr<RemoteSession> server_session;

    std::thread server_thread([&] {
        std::string server_error;
        auto session = accept_and_serve(listener.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        if (!session) return;
        session->set_control_request_handler(
            [](const KopmsControlCommandPayload& command,
               const std::vector<uint8_t>& /*data*/,
               KopmsControlAckPayload* ack) -> KopmsControlStatus {
                ack->generation = command.object_id + 1;
                return KOPMS_CONTROL_STATUS_OK;
            });
        std::lock_guard<std::mutex> lock(server_mutex);
        server_session = std::move(session);
        server_session->wait_done(8000);
    });

    const uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF |
                                 KOPMS_PROTOCOL_CAP_CONTROL_STATE;
    RemoteSession client;
    CHECK(dial_and_hello(&client, path, kClientCaps, &error), error.c_str());

    KopmsControlCommandPayload command{};
    command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
    command.operation = KOPMS_CONTROL_WINDOW_CREATE;
    command.object_id = 42;
    command.related_id = 0;
    command.value = 1;
    command.flags = 0;
    command.data_size = 0;
    uint64_t request_sequence = 0;
    CHECK(client.send_control(command, {}, &request_sequence, &error),
          error.c_str());
    CHECK(request_sequence != 0, "send_control 应返回请求序列号");
    KopmsControlAckPayload ack{};
    CHECK(client.wait_for_control_ack(request_sequence, 5000, &ack, &error),
          error.c_str());
    CHECK(ack.status == KOPMS_CONTROL_STATUS_OK, "CONTROL_ACK 状态应为 OK");
    CHECK(ack.object_id == 42, "CONTROL_ACK 应回填 object_id");
    CHECK(ack.generation == 43, "CONTROL_ACK 应回填 handler 写入的 generation");
    CHECK(ack.request_sequence == request_sequence, "CONTROL_ACK 应回填请求序列号");

    client.disconnect();
    server_thread.join();
    return g_failures == 0;
}

bool test_protocol_error_lane() {
    // 向 control lane 注入未知消息类型：对端应回 ERROR 并结束会话。
    const std::string path = "/tmp/kopnet-remote-error.sock";
    ::unlink(path.c_str());
    std::string error;
    std::string bind_error;
    auto listener = bind_unix(path, &bind_error);
    CHECK(listener != nullptr, bind_error.c_str());
    if (!listener) return false;

    std::mutex server_mutex;
    std::unique_ptr<RemoteSession> server_session;
    std::atomic<int> server_errors{0};

    std::thread server_thread([&] {
        std::string server_error;
        auto session = accept_and_serve(listener.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        if (!session) return;
        session->set_error_observer(
            [&](KopmsErrorCode /*code*/, const std::string& /*message*/) {
                server_errors.fetch_add(1);
            });
        std::lock_guard<std::mutex> lock(server_mutex);
        server_session = std::move(session);
        server_session->wait_done(8000);
    });

    const uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF;
    RemoteSession client;
    CHECK(dial_and_hello(&client, path, kClientCaps, &error), error.c_str());
    std::atomic<int> client_errors{0};
    auto count_error = [&]() { client_errors.fetch_add(1); };
    client.set_error_observer([&](KopmsErrorCode /*code*/,
                                 const std::string& /*message*/) {
        count_error();
    });

    KopmsMessageHeader header{};
    header.magic = KOPMS_PROTOCOL_MAGIC;
    header.major = KOPMS_PROTOCOL_MAJOR;
    header.minor = KOPMS_PROTOCOL_MINOR;
    header.type = 0xffff;  // 未知类型
    header.flags = KOPMS_MESSAGE_FLAG_CONTROL;
    header.sequence = 2;  // 严格递增（HELLO 已占用 1）
    header.payload_size = 0;
    header.fd_count = 0;
    kopms::WireHeader wire{};
    kopms::encode_message_header(header, &wire);
    const std::vector<uint8_t> bytes(wire.begin(), wire.end());
    CHECK(client.send_raw_control(bytes, &error), error.c_str());

    CHECK(client.wait_done(5000), "对端 ERROR 后会话应结束");
    CHECK(client_errors.load() >= 1, "connect 侧应收到 ERROR 通知");

    client.disconnect();
    server_thread.join();
    return g_failures == 0;
}

// 断线自动重连 + Endpoint 故障转移：主服务端被杀后，客户端自动切到备用端点，
// 同一 RemoteSession 对象重新 hello 并继续提交帧；在途帧被判为 DROPPED。
// 断线自动重连 + Endpoint 故障转移：主服务端被杀后，客户端自动切到备用端点，
// 同一 RemoteSession 对象重新 hello 并继续提交帧；在途帧被判为 DROPPED。
bool test_reconnect_and_failover() {
    const std::string path_a = "/tmp/kopnet-remote-rc-a.sock";
    const std::string path_b = "/tmp/kopnet-remote-rc-b.sock";
    ::unlink(path_a.c_str());
    ::unlink(path_b.c_str());
    std::string error;
    std::string bind_error;
    auto listener_a = bind_unix(path_a, &bind_error);
    CHECK(listener_a != nullptr, bind_error.c_str());
    if (!listener_a) return false;
    auto listener_b = bind_unix(path_b, &bind_error);
    CHECK(listener_b != nullptr, bind_error.c_str());
    if (!listener_b) return false;

    // 服务端会话由主线程与服务端线程共享所有权：服务端可能因传输断开自行
    // 结束，主线程持 shared_ptr 才能安全地对它 disconnect()。
    std::mutex server_mu;
    std::shared_ptr<RemoteSession> server_a;
    std::shared_ptr<RemoteSession> server_b;
    std::atomic<uint32_t> a_frame_id{0};  // A 侧 Retain 住的帧 id
    RemoteFrame retained_a;

    std::thread thread_a([&] {
        std::string server_error;
        auto session = accept_and_serve(listener_a.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        if (!session) return;
        session->set_frame_handler([&](RemoteFrame frame) -> FrameDisposition {
            a_frame_id.store(frame.payload.frame_id);
            retained_a = std::move(frame);
            return FrameDisposition::Retain;
        });
        auto shared = std::shared_ptr<RemoteSession>(session.release());
        {
            std::lock_guard<std::mutex> lock(server_mu);
            server_a = shared;
        }
        shared->wait_done(15000);
    });

    std::thread thread_b([&] {
        std::string server_error;
        auto session = accept_and_serve(listener_b.get(), &server_error);
        CHECK(session != nullptr, server_error.c_str());
        if (!session) return;
        auto shared = std::shared_ptr<RemoteSession>(session.release());
        {
            std::lock_guard<std::mutex> lock(server_mu);
            server_b = shared;
        }
        shared->wait_done(15000);
    });

    const uint64_t kClientCaps = KOPMS_PROTOCOL_CAP_DMABUF |
                                 KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                                 KOPMS_PROTOCOL_CAP_COLOR_METADATA;
    RemoteSession client;
    std::atomic<int> reconnects{0};
    std::atomic<int> dropped{0};
    client.set_reconnect_observer([&] { reconnects.fetch_add(1); });
    client.set_frame_release_observer(
        [&](uint32_t /*frame_id*/, uint32_t status) {
            if (status == KOPMS_FRAME_RELEASE_DROPPED) dropped.fetch_add(1);
        });

    RemoteSession::Options opts;
    opts.hello_timeout_ms = 5000;
    opts.auto_reconnect = true;
    opts.fallback_endpoints = {"unix://" + path_b};
    opts.reconnect_base_delay_ms = 100;
    opts.reconnect_max_delay_ms = 400;
    CHECK(client.open("unix://" + path_a, opts, &error), error.c_str());
    CHECK(client.hello(kClientCaps, &error), error.c_str());
    CHECK(client.ping(&error), error.c_str());

    std::shared_ptr<RemoteSession> a_snapshot;
    {
        std::lock_guard<std::mutex> lock(server_mu);
        a_snapshot = server_a;
    }
    CHECK(a_snapshot != nullptr, "服务端 A 未建立");

    // 提交一帧给 A：A 会 Retain，故这帧一直在途
    const int fd1 = make_memfd(kPixelMagic, std::strlen(kPixelMagic));
    CHECK(fd1 >= 0, "构造 memfd 失败");
    KopmsFrameDescriptor desc1 = make_descriptor(fd1);
    uint32_t frame1 = 0;
    CHECK(client.submit(&desc1, &frame1, &error), error.c_str());
    CHECK(frame1 != 0, "submit 应返回非零 frame_id");

    const auto t0 = std::chrono::steady_clock::now();
    while (a_frame_id.load() != frame1 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(a_frame_id.load() == frame1, "服务端 A 未在超时内收到帧");

    // 杀掉 A：先关监听器（使重连拨 A 必失败），再断已接受的会话
    listener_a.reset();
    a_snapshot->disconnect();

    // 等待故障转移到 B 并触发重连回调
    const auto t1 = std::chrono::steady_clock::now();
    while (reconnects.load() == 0 &&
           std::chrono::steady_clock::now() - t1 < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(reconnects.load() > 0, "未在超时内完成故障转移/重连");

    // 在途帧应被判为 DROPPED
    CHECK(dropped.load() >= 1, "断开时在途帧应回放 DROPPED");
    // 重新协商前 submit 应失败
    CHECK(!client.submit(&desc1, &frame1, &error), "未重新 hello 时 submit 不应成功");

    // 重新协商并继续工作：同一对象、同一逻辑通道
    CHECK(client.hello(kClientCaps, &error), error.c_str());
    CHECK(client.ping(&error), error.c_str());
    const int fd2 = make_memfd(kPixelMagic, std::strlen(kPixelMagic));
    CHECK(fd2 >= 0, "构造第二个 memfd 失败");
    KopmsFrameDescriptor desc2 = make_descriptor(fd2);
    uint32_t frame2 = 0;
    CHECK(client.submit(&desc2, &frame2, &error), error.c_str());
    CHECK(frame2 != frame1, "重连后 frame_id 不应回卷");
    CHECK(client.wait_for_release(frame2, 5000, &error), error.c_str());

    client.disconnect();
    thread_a.join();
    thread_b.join();
    {
        std::lock_guard<std::mutex> lock(server_mu);
        server_a.reset();
        server_b.reset();
    }
    retained_a.close_fds();
    return g_failures == 0;
}

bool test_argument_guards() {
    std::string error;
    RemoteSession client;
    CHECK(!client.submit(nullptr, nullptr, &error), "未连接时 submit 应失败");
    CHECK(!client.ping(&error), "未连接时 ping 应失败");

    RemoteSession server;
    CHECK(!server.serve(nullptr, RemoteSession::Options(), &error), "空会话应拒绝");
    CHECK(!server.release_frame(1, KOPMS_FRAME_RELEASE_OK, &error),
          "非 serve 角色的 release_frame 应失败");
    return g_failures == 0;
}

}  // namespace

int main() {
    const int failures_before = g_failures;
    if (!test_argument_guards()) g_failures++;
    if (!test_handshake_and_ping()) g_failures++;
    if (!test_frame_roundtrip_with_fds()) g_failures++;
    if (!test_control_request_and_ack()) g_failures++;
    if (!test_protocol_error_lane()) g_failures++;
    if (!test_reconnect_and_failover()) g_failures++;
    if (g_failures == failures_before) {
        std::printf("kopnet-remote: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "kopnet-remote: %d failure(s)\n", g_failures);
    return 1;
}
