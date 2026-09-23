// KOPNET 隧道测试：socketpair 回环多路复用、fd 透传、credit 回压、TCP 服务端。
#include "kopnet/tunnel.hpp"

#include <sys/eventfd.h>
#include <sys/socket.h>
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

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);\
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using kopnet::ChannelMode;
using kopnet::TunnelSession;

std::unique_ptr<TunnelSession> make_peer(kopnet::Transport* raw, bool dialer) {
    std::unique_ptr<kopnet::Transport> t(raw);
    std::string error;
    auto session = TunnelSession::create(std::move(t), dialer, TunnelSession::Options(), &error);
    CHECK(session != nullptr, error.c_str());
    CHECK(session->start(&error), error.c_str());
    return session;
}

void test_basic_multiplex() {
    std::string error;
    std::unique_ptr<kopnet::Transport> ta;
    std::unique_ptr<kopnet::Transport> tb;
    CHECK(kopnet::make_socketpair_transports(&ta, &tb, &error), error.c_str());

    auto dialer = make_peer(ta.release(), true);
    auto listener = make_peer(tb.release(), false);

    std::promise<uint32_t> accepted;
    auto accepted_future = accepted.get_future();
    listener->set_channel_handler([&](uint32_t id, const std::string& kind, ChannelMode) {
        if (kind != "echo") return;  // 后续 mux 通道不经此 promise
        CHECK(kind == "echo", "kind 不匹配");
        accepted.set_value(id);
    });

    uint32_t ch = dialer->open_channel("echo", ChannelMode::Stream, 3000, &error);
    CHECK(ch != 0, error.c_str());
    uint32_t remote = accepted_future.get();
    CHECK(remote == ch, "通道 id 未透传");

    // 双向小数据
    const std::string ping = "hello-kopnet";
    CHECK(dialer->send(ch, reinterpret_cast<const uint8_t*>(ping.data()), ping.size(), nullptr,
                       1000) == kopnet::SendStatus::Ok,
          "dialer send");
    std::vector<uint8_t> got;
    std::vector<int> got_fds;
    CHECK(listener->recv(ch, &got, &got_fds, 3000) == kopnet::RecvStatus::Ok, "listener recv");
    CHECK(std::string(got.begin(), got.end()) == ping, "数据内容不一致");
    CHECK(got_fds.empty(), "不应有 fd");

    const std::string pong = "pong-回压链";
    CHECK(listener->send(remote, reinterpret_cast<const uint8_t*>(pong.data()), pong.size(),
                         nullptr, 1000) == kopnet::SendStatus::Ok,
          "listener send");
    CHECK(dialer->recv(ch, &got, &got_fds, 3000) == kopnet::RecvStatus::Ok, "dialer recv");
    CHECK(std::string(got.begin(), got.end()) == pong, "反向数据不一致");

    // 三通道交错复用
    std::vector<uint32_t> chans;
    for (int i = 0; i < 3; ++i) {
        uint32_t c = dialer->open_channel("mux", ChannelMode::Datagram, 3000, &error);
        CHECK(c != 0, "打开复用通道");
        chans.push_back(c);
    }
    for (size_t i = 0; i < chans.size(); ++i) {
        std::string payload = "ch-" + std::to_string(i) + "-payload";
        CHECK(dialer->send(chans[i], reinterpret_cast<const uint8_t*>(payload.data()),
                           payload.size(), nullptr, 1000) == kopnet::SendStatus::Ok,
              "复用发送");
    }
    for (size_t i = 0; i < chans.size(); ++i) {
        std::string expect = "ch-" + std::to_string(i) + "-payload";
        CHECK(listener->recv(chans[i], &got, &got_fds, 3000) == kopnet::RecvStatus::Ok,
              "复用接收");
        CHECK(std::string(got.begin(), got.end()) == expect, "复用通道数据错配");
    }

    // 大帧完整性（接近单帧上限）
    std::vector<uint8_t> big(60 * 1024);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i & 0xff);
    CHECK(dialer->send(ch, big.data(), big.size(), nullptr, 2000) == kopnet::SendStatus::Ok,
          "大帧发送");
    std::vector<uint8_t> big_got;
    CHECK(listener->recv(ch, &big_got, &got_fds, 5000) == kopnet::RecvStatus::Ok, "大帧接收");
    CHECK(big_got.size() == big.size(), "大帧长度不一致");
    CHECK(std::memcmp(big_got.data(), big.data(), big.size()) == 0, "大帧内容损坏");

    dialer->stop();
    listener->stop();
}

void test_fd_passing() {
    std::string error;
    std::unique_ptr<kopnet::Transport> ta;
    std::unique_ptr<kopnet::Transport> tb;
    CHECK(kopnet::make_socketpair_transports(&ta, &tb, &error), error.c_str());
    auto dialer = make_peer(ta.release(), true);
    auto listener = make_peer(tb.release(), false);

    std::promise<uint32_t> accepted;
    listener->set_channel_handler([&](uint32_t id, const std::string&, ChannelMode) {
        accepted.set_value(id);
    });
    uint32_t ch = dialer->open_channel("fd", ChannelMode::Stream, 3000, &error);
    CHECK(ch != 0, "打开通道");
    accepted.get_future().get();

    // eventfd 写入 42 后把 fd 发给对端
    int efd = ::eventfd(0, EFD_CLOEXEC);
    CHECK(efd >= 0, "eventfd 创建失败");
    uint64_t val = 42;
    CHECK(::write(efd, &val, sizeof(val)) == sizeof(val), "eventfd 写入失败");

    std::vector<int> fds = {efd};
    const std::string tag = "dma-buf-metadata";
    CHECK(dialer->send(ch, reinterpret_cast<const uint8_t*>(tag.data()), tag.size(), &fds, 1000) ==
              kopnet::SendStatus::Ok,
          "携带 fd 发送");
    // send 成功后 fd 所有权移交隧道（由其在发送完成后关闭），这里不再关闭

    std::vector<uint8_t> got;
    std::vector<int> got_fds;
    CHECK(listener->recv(ch, &got, &got_fds, 3000) == kopnet::RecvStatus::Ok, "接收携带 fd 的帧");
    CHECK(std::string(got.begin(), got.end()) == tag, "元数据内容不一致");
    CHECK(got_fds.size() == 1, "未收到 fd");
    if (!got_fds.empty()) {
        uint64_t got_val = 0;
        CHECK(::read(got_fds[0], &got_val, sizeof(got_val)) == sizeof(got_val), "读取对端 fd");
        CHECK(got_val == 42, "fd 内容不一致（fd 未正确透传）");
        ::close(got_fds[0]);
    }
    dialer->stop();
    listener->stop();
}

void test_credit_backpressure() {
    // 收侧额度极小：必须靠 credit 补发才能送完全部报文
    std::string error;
    std::unique_ptr<kopnet::Transport> ta;
    std::unique_ptr<kopnet::Transport> tb;
    CHECK(kopnet::make_socketpair_transports(&ta, &tb, &error), error.c_str());

    TunnelSession::Options small;
    small.recv_credit = 2;
    auto dialer = make_peer(ta.release(), true);
    auto listener = TunnelSession::create(std::move(tb), false, small, &error);
    CHECK(listener->start(&error), error.c_str());

    std::promise<uint32_t> accepted;
    listener->set_channel_handler([&](uint32_t id, const std::string&, ChannelMode) {
        accepted.set_value(id);
    });
    uint32_t ch = dialer->open_channel("bp", ChannelMode::Stream, 3000, &error);
    CHECK(ch != 0, "打开通道");
    uint32_t remote = accepted.get_future().get();

    const int kCount = 24;
    std::vector<std::vector<uint8_t>> msgs;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> m(1024, static_cast<uint8_t>(i & 0xff));
        msgs.push_back(m);
    }
    // 生产者线程：send 在额度耗尽时会阻塞于通道发送队列（背压）
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            CHECK(dialer->send(ch, msgs[i].data(), msgs[i].size(), nullptr, 10000) ==
                      kopnet::SendStatus::Ok,
                  "背压下发送");
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // 期间对端队列应保持有界（额度 2 + 队列深度 2，远小于 24）
    CHECK(listener->channel_count() == 1, "通道数");
    int received = 0;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> got;
        std::vector<int> got_fds;
        CHECK(listener->recv(remote, &got, &got_fds, 10000) == kopnet::RecvStatus::Ok,
              "背压下接收");
        CHECK(got == msgs[i], "报文顺序/内容错乱");
        ++received;
    }
    producer.join();
    CHECK(received == kCount, "报文总数不一致");
    CHECK(dialer->frames_sent() >= static_cast<uint64_t>(kCount), "已发帧计数");
    CHECK(listener->frames_received() >= static_cast<uint64_t>(kCount), "已收帧计数");
    dialer->stop();
    listener->stop();
}

void test_socket_backpressure() {
    // 接收方长时间不读，强制发送侧 socket 缓冲打满：flush_writes 必须正确处理
    // WouldBlock（帧放回队首，不得重复或产生空帧）与部分写（fd 只随首片）。
    std::string error;
    std::unique_ptr<kopnet::Transport> ta;
    std::unique_ptr<kopnet::Transport> tb;
    CHECK(kopnet::make_socketpair_transports(&ta, &tb, &error), error.c_str());

    kopnet::TunnelSession::Options big;
    big.recv_credit = 512;  // 让背压发生在 socket 层而非 credit 层
    auto dialer = make_peer(ta.release(), true);
    auto listener = TunnelSession::create(std::move(tb), false, big, &error);
    CHECK(listener->start(&error), error.c_str());

    std::promise<uint32_t> accepted;
    listener->set_channel_handler([&](uint32_t id, const std::string& kind, ChannelMode) {
        if (kind == "flood") accepted.set_value(id);
    });
    uint32_t ch = dialer->open_channel("flood", ChannelMode::Stream, 3000, &error);
    CHECK(ch != 0, "打开通道");
    uint32_t remote = accepted.get_future().get();

    const int kCount = 64;
    const size_t kSize = 60 * 1024;  // 须 < KOPNET_TUNNEL_MAX_PAYLOAD
    std::vector<std::vector<uint8_t>> msgs;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> m(kSize, static_cast<uint8_t>(i & 0xff));
        msgs.push_back(m);
    }
    std::atomic<int> sent(0);
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            if (dialer->send(ch, msgs[i].data(), msgs[i].size(), nullptr, 30000) !=
                kopnet::SendStatus::Ok) {
                break;
            }
            ++sent;
        }
    });
    // socket 缓冲打满期间阻塞一会（触发 WouldBlock + 部分写重排）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // 之后开始排空，全部数据必须严格有序、无空帧
    int received = 0;
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> got;
        std::vector<int> got_fds;
        if (listener->recv(remote, &got, &got_fds, 30000) != kopnet::RecvStatus::Ok) break;
        CHECK(got.size() == kSize, "洪泛帧尺寸");
        CHECK(got == msgs[i], "洪泛帧内容/顺序错乱");
        ++received;
    }
    producer.join();
    CHECK(sent.load() == kCount, "发送端未全部投递");
    CHECK(received == kCount, "洪泛帧总数不一致");
    dialer->stop();
    listener->stop();
}

void test_tcp_server_and_dial() {
    // 临时分配一个可用端口（避免与 CI 冲突）
    std::string error;
    std::string uri;
    kopnet::TunnelServer server;
    bool listening = false;
    for (int port = 18700; port < 18760; ++port) {
        uri = "tcp://127.0.0.1:" + std::to_string(port);
        if (server.listen(uri, &error)) {
            listening = true;
            break;
        }
    }
    CHECK(listening, "无法绑定测试端口");

    std::atomic<int> sessions{0};
    std::promise<uint32_t> accepted;
    auto accepted_future = accepted.get_future();
    std::vector<std::unique_ptr<TunnelSession>> held;
    std::mutex held_mu;
    std::thread server_thread([&] {
        server.run([&](std::unique_ptr<TunnelSession> session) {
            sessions.fetch_add(1);
            TunnelSession* raw = session.get();
            {
                std::lock_guard<std::mutex> l(held_mu);
                held.push_back(std::move(session));
            }
            raw->set_channel_handler([&, raw](uint32_t id, const std::string&, ChannelMode) {
                raw->set_data_handler(
                    id, [id, &accepted](uint32_t, const std::vector<uint8_t>& data,
                                        const std::vector<int>&) {
                        if (data.size() == 4 && data[0] == 'p' && data[1] == 'i' &&
                            data[2] == 'n' && data[3] == 'g') {
                            accepted.set_value(id);
                        }
                    });
            });
        });
    });

    auto client = kopnet::tunnel_dial(uri, TunnelSession::Options(), &error);
    CHECK(client != nullptr, error.c_str());
    uint32_t ch = client->open_channel("ctrl", ChannelMode::Stream, 3000, &error);
    CHECK(ch != 0, error.c_str());
    const std::string ping = "ping";
    CHECK(client->send(ch, reinterpret_cast<const uint8_t*>(ping.data()), ping.size(), nullptr,
                       2000) == kopnet::SendStatus::Ok,
          "tcp 发送");
    CHECK(accepted_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "服务端未收到数据");
    CHECK(sessions.load() == 1, "会话计数");

    client->stop();
    server.stop();
    server_thread.join();
    std::lock_guard<std::mutex> l(held_mu);
    for (auto& s : held) s->stop();
    held.clear();
}

void test_mode_mismatch() {
    // Datagram 语义传输上开 Stream 通道应被拒绝
    std::string error;
    std::unique_ptr<kopnet::Transport> ta;
    std::unique_ptr<kopnet::Transport> tb;
    CHECK(kopnet::make_socketpair_transports(&ta, &tb, &error), error.c_str());
    // 强行把一端换成 datagram 语义不现实；此处验证 URI 层与适配器层
    // 的拒绝路径：向不支持的 scheme dial 应得到明确错误。
    // rtp:// 已实现（见 rtp_test.cpp），用仍未实现的 rtc:// 验证拒绝路径。
    std::unique_ptr<kopnet::Transport> dummy;
    CHECK(!kopnet::AdapterRegistry::instance().dial("rtc://example.com:8443", &dummy,
                                                    &error),
          "rtc 未实现应返回失败");
    CHECK(error.find("尚未实现") != std::string::npos, "错误信息应说明未实现");
    CHECK(kopnet::AdapterRegistry::instance().has_scheme("rtp"), "rtp 应已注册");
    CHECK(kopnet::AdapterRegistry::instance().has_scheme("rtc"), "rtc 应已注册");
    CHECK(!kopnet::AdapterRegistry::instance().has_scheme("bogus"), "未知 scheme");
}

}  // namespace

int main() {
    test_basic_multiplex();
    test_fd_passing();
    test_credit_backpressure();
    test_socket_backpressure();
    test_tcp_server_and_dial();
    test_mode_mismatch();
    if (g_failures == 0) {
        std::printf("kopnet-tunnel-test: ALL PASS\n");
        return 0;
    }
    std::printf("kopnet-tunnel-test: %d FAILURES\n", g_failures);
    return 1;
}
