// KOPNET 会话韧性测试：断线自动重连、Endpoint 列表故障转移、逻辑通道跨重连保持、
// recv 跨断连挂起、重连次数耗尽。
#include "kopnet/resilient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kopnet/adapters.hpp"
#include "kopnet/tunnel.hpp"

namespace {

std::atomic<int> g_failures{0};

#define CHECK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            g_failures.fetch_add(1);                                           \
        }                                                                      \
    } while (0)

using kopnet::ChannelMode;
using kopnet::RecvStatus;
using kopnet::ResilientSession;
using kopnet::SendStatus;
using kopnet::TunnelServer;
using kopnet::TunnelSession;

// 探测一个空闲端口（bind 临时套接字后立即关闭）
int pick_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket 创建失败");
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0, "bind 失败");
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0,
          "getsockname 失败");
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// 回显服务端：每条接入会话的每个通道都把数据原样发回
class EchoServer {
public:
    bool start(int port, std::string* error) {
        server_ = std::make_unique<TunnelServer>();
        uri_ = "tcp://127.0.0.1:" + std::to_string(port);
        if (!server_->listen(uri_, error)) return false;
        thread_ = std::thread([this] {
            server_->run([&](std::unique_ptr<TunnelSession> session) {
                TunnelSession* raw = session.get();
                {
                    std::lock_guard<std::mutex> l(mu_);
                    held_.push_back(std::move(session));
                }
                raw->set_channel_handler([raw](uint32_t id, const std::string&, ChannelMode) {
                    raw->set_data_handler(
                        id, [raw, id](uint32_t, const std::vector<uint8_t>& data,
                                      const std::vector<int>& fds) {
                            raw->send(id, data.data(), data.size(),
                                      fds.empty() ? nullptr : &fds, 2000);
                        });
                });
            });
        });
        return true;
    }

    void stop() {
        if (server_) server_->stop();
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> l(mu_);
        for (auto& s : held_) s->stop();
        held_.clear();
    }

    ~EchoServer() { stop(); }

private:
    std::unique_ptr<TunnelServer> server_;
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::unique_ptr<TunnelSession>> held_;
    std::string uri_;
};

bool roundtrip(ResilientSession& s, uint32_t ch, const std::string& msg) {
    if (s.send(ch, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), nullptr, 2000) !=
        SendStatus::Ok) {
        std::fprintf(stderr, "roundtrip: send 失败（%s）\n", msg.c_str());
        return false;
    }
    std::vector<uint8_t> data;
    std::vector<int> fds;
    if (s.recv(ch, &data, &fds, 4000) != RecvStatus::Ok) {
        std::fprintf(stderr, "roundtrip: recv 失败（%s）\n", msg.c_str());
        return false;
    }
    const std::string got(data.begin(), data.end());
    if (got != msg) {
        std::fprintf(stderr, "roundtrip: 内容不符（%s vs %s）\n", got.c_str(), msg.c_str());
        return false;
    }
    return true;
}

// 主路径：断开 → 自动故障转移 → 同一逻辑通道恢复通信
void test_reconnect_and_failover() {
    const int port_a = pick_port();
    const int port_b = pick_port();
    CHECK(port_a != port_b, "两个端口不应相同");

    EchoServer server_a;
    std::string error;
    CHECK(server_a.start(port_a, &error), error.c_str());

    ResilientSession::Options opts;
    opts.endpoints = {"tcp://127.0.0.1:" + std::to_string(port_a),
                      "tcp://127.0.0.1:" + std::to_string(port_b)};
    opts.base_delay_ms = 200;
    opts.max_delay_ms = 1000;
    opts.connect_timeout_ms = 2000;

    ResilientSession client(opts);
    std::atomic<int> opens{0};
    std::atomic<int> connects{0};
    std::atomic<int> disconnects{0};
    client.set_channel_handler(
        [&](uint32_t, const std::string& kind, ChannelMode) { if (kind == "echo") ++opens; });
    client.set_state_handler([&](bool connected) { connected ? ++connects : ++disconnects; });

    CHECK(client.start(&error), error.c_str());
    const uint32_t ch = client.open_channel("echo", ChannelMode::Stream, &error);
    CHECK(ch != 0, "open_channel 应返回逻辑号");
    CHECK(client.wait_connected(5000), "首次连接超时");
    CHECK(roundtrip(client, ch, "ping-1"), "首段往返");
    CHECK(opens.load() >= 1, "通道回调应触发");
    CHECK(connects.load() >= 1, "状态回调应通知已连接");

    // 杀掉主服务端：模拟传输断开
    server_a.stop();
    CHECK(wait_until([&] { return !client.connected(); }, 3000), "断开后 connected 应翻为 false");

    const std::string probe = "probe-while-down";
    CHECK(client.send(ch, reinterpret_cast<const uint8_t*>(probe.data()), probe.size(), nullptr,
                      500) == SendStatus::Closed,
          "断开期间 send 应立即返回 Closed");
    CHECK(disconnects.load() >= 1, "状态回调应通知断开");

    // 起备用服务端：客户端应自动故障转移过去
    EchoServer server_b;
    CHECK(server_b.start(port_b, &error), error.c_str());
    CHECK(wait_until([&] { return client.connected(); }, 6000), "故障转移到备用端点超时");
    CHECK(connects.load() >= 2, "应至少连接两次");

    // 同一个逻辑通道号继续可用
    CHECK(roundtrip(client, ch, "ping-2"), "重连后同逻辑通道往返");
    CHECK(opens.load() >= 2, "通道应在重连后被重新打开");

    client.stop();
    server_b.stop();
}

// 断连期间阻塞的 recv 在重连后自动恢复；断连期间注册的通道由连接线程开
void test_recv_parks_across_outage() {
    const int port = pick_port();
    EchoServer server;
    std::string error;
    CHECK(server.start(port, &error), error.c_str());

    ResilientSession::Options opts;
    opts.endpoints = {"tcp://127.0.0.1:" + std::to_string(port)};
    opts.base_delay_ms = 150;
    opts.max_delay_ms = 800;
    opts.connect_timeout_ms = 2000;
    ResilientSession client(opts);
    CHECK(client.start(&error), error.c_str());
    const uint32_t ch = client.open_channel("echo", ChannelMode::Stream, &error);
    CHECK(client.wait_connected(5000), "首次连接超时");
    CHECK(roundtrip(client, ch, "warmup"), "预热往返");

    // 在断连之前先挂一个 recv（长超时），它应当跨越整个断连期存活
    std::atomic<bool> got{false};
    std::string mismatch;
    std::thread waiter([&] {
        std::vector<uint8_t> data;
        std::vector<int> fds;
        if (client.recv(ch, &data, &fds, 10000) != RecvStatus::Ok) {
            mismatch = "recv 未在重连后返回数据";
            return;
        }
        const std::string s(data.begin(), data.end());
        if (s != "after-reconnect") mismatch = "内容不符: " + s;
        got.store(true);
    });

    // 断开，等客户端感知，再重启服务端
    server.stop();
    CHECK(wait_until([&] { return !client.connected(); }, 3000), "未感知到断开");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(server.start(port, &error), error.c_str());
    CHECK(client.wait_connected(6000), "重连超时");

    // 断连期间注册的第二通道：连接线程应在重连后自动打开
    const uint32_t ch2 = client.open_channel("echo", ChannelMode::Stream, &error);
    CHECK(ch2 != 0, "断连期间应能注册通道");
    CHECK(roundtrip(client, ch2, "second-channel"), "断连期间注册的通道应可用");

    CHECK(client.send(ch, reinterpret_cast<const uint8_t*>("after-reconnect"), 15, nullptr, 2000) ==
              SendStatus::Ok,
          "发送");
    waiter.join();
    CHECK(got.load(), mismatch.empty() ? "recv 未恢复" : mismatch.c_str());

    client.stop();
    server.stop();
}

// 重连次数耗尽后会话停止，wait_connected 返回 false
void test_max_attempts_exhausted() {
    const int port = pick_port();  // 无人监听：拨号必然失败
    ResilientSession::Options opts;
    opts.endpoints = {"tcp://127.0.0.1:" + std::to_string(port)};
    opts.base_delay_ms = 100;
    opts.max_delay_ms = 300;
    opts.connect_timeout_ms = 500;
    opts.max_attempts = 2;
    ResilientSession client(opts);
    std::string error;
    CHECK(client.start(&error), error.c_str());
    CHECK(!client.wait_connected(2500), "无人服务时应连不上");
    CHECK(client.open_channel("echo", ChannelMode::Stream, &error) != 0, "仍应能注册通道");
    CHECK(wait_until([&] { return !client.connected(); }, 1000), "耗尽后应保持断开");
    client.stop();
}

}  // namespace

int main() {
    test_reconnect_and_failover();
    test_recv_parks_across_outage();
    test_max_attempts_exhausted();
    if (g_failures.load() == 0) {
        std::printf("kopnet-resilient: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "kopnet-resilient: %d FAILURES\n", g_failures.load());
    return 1;
}
