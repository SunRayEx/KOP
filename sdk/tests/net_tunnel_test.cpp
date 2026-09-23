// KOP App SDK —— NetTunnel facade 测试。
//
// 覆盖：
//   1. tcp:// 回显：拨号 + 服务端双向通信、payload 一致、on_channel/stats
//   2. 多通道：Stream（ordered）与 Datagram 共存，按 kind 路由
//   3. 服务端不能主动开通道（契约校验）
//   4. unix:// 自动重连：服务端销毁后拨号端自动重连并恢复逻辑通道
//   5. 关闭自动重连时断线即终止
//   6. close_channel 后发送得到明确错误
//
// 用 kop::sdk::TestReport 输出 [PASS]/[FAIL]，退出码 = 失败数。
#include "kop/app_sdk.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kop::sdk;

namespace {

// 线程安全的消息收集箱：on_data 往里塞，测试线程按 kind 取
struct MessageBox {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> msgs;

    void push(const std::string& kind, const uint8_t* data, size_t len) {
        {
            std::lock_guard<std::mutex> lk(mu);
            msgs.emplace_back(kind, std::vector<uint8_t>(data, data + len));
        }
        cv.notify_all();
    }

    bool wait_for(const std::string& kind, int timeout_ms, std::vector<uint8_t>* out) {
        std::unique_lock<std::mutex> lk(mu);
        const bool got = cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            for (const auto& m : msgs) {
                if (m.first == kind) return true;
            }
            return false;
        });
        if (!got) return false;
        for (const auto& m : msgs) {
            if (m.first == kind) {
                if (out) *out = m.second;
                return true;
            }
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        msgs.clear();
    }
};

// 带重试的发送：刚重连完成时逻辑通道可能尚未重开，给一点宽限
bool try_send(NetTunnel& tunnel, const std::string& kind, const std::vector<uint8_t>& data,
              int deadline_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string err;
        if (tunnel.send(kind, data.data(), data.size(), &err)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// 回显服务端：收到的数据原样发回同 kind 通道
NetTunnelOptions echo_server_options(const std::string& uri, MessageBox* box) {
    NetTunnelOptions opts;
    opts.uri = uri;
    opts.serve = true;
    opts.on_data = [box](const std::string& kind, const uint8_t* data, size_t len) {
        box->push(kind, data, len);
    };
    return opts;
}

// 状态记账：记录当前是否连通 + 断开事件次数（一次断开可能很快被重连追平）
struct LinkState {
    std::atomic<bool> connected{false};
    std::atomic<int> disconnects{0};
};

// 拨号端
NetTunnelOptions dialer_options(const std::string& uri, MessageBox* box,
                               LinkState* state = nullptr,
                               bool auto_reconnect = true,
                               uint32_t reconnect_base_ms = 300) {
    NetTunnelOptions opts;
    opts.uri = uri;
    opts.serve = false;
    opts.auto_reconnect = auto_reconnect;
    opts.reconnect_base_ms = reconnect_base_ms;
    opts.reconnect_max_ms = 2000;
    opts.handshake_ms = 5000;
    opts.on_data = [box](const std::string& kind, const uint8_t* data, size_t len) {
        box->push(kind, data, len);
    };
    if (state) {
        opts.on_state = [state](bool connected) {
            if (connected) {
                state->connected.store(true);
            } else {
                state->connected.store(false);
                state->disconnects.fetch_add(1);
            }
        };
    }
    return opts;
}

void test_echo(TestReport& report) {
    report.section("tcp 回显");
    const std::string uri = "tcp://127.0.0.1:19710";
    MessageBox server_box, client_box;
    std::atomic<int> server_channels(0);

    NetTunnelOptions sopts = echo_server_options(uri, &server_box);
    sopts.on_channel = [&server_channels](const std::string& kind) {
        if (kind == "media") server_channels.fetch_add(1);
    };
    std::string err;
    auto server = NetTunnel::open(sopts, &err);
    report.check(server != nullptr, "服务端 open", err);

    auto client = NetTunnel::open(dialer_options(uri, &client_box), &err);
    report.check(client != nullptr, "拨号端 open", err);
    if (!server || !client) return;

    report.check(client->wait_connected(5000), "拨号端等首连");
    const uint32_t ch = client->open_channel("media", false, &err);
    report.check(ch > 0, "打开 media 通道", err);
    if (ch == 0) return;

    // 服务端应看到对端通道打开
    for (int i = 0; i < 100 && server_channels.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report.check(server_channels.load() > 0, "服务端 on_channel 收到 media");

    // 客户端 → 服务端
    const std::vector<uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
    report.check(try_send(*client, "media", hello, 3000), "客户端发送");
    std::vector<uint8_t> got;
    report.check(server_box.wait_for("media", 3000, &got) && got == hello,
                 "服务端收到完整 payload");

    // 服务端 → 客户端（用对端打开的同名通道）
    report.check(try_send(*server, "media", hello, 3000), "服务端发送");
    got.clear();
    report.check(client_box.wait_for("media", 3000, &got) && got == hello,
                 "客户端收到完整 payload");

    const NetTunnelStats st = client->stats();
    report.check(st.connected, "stats.connected 为真");
    report.check(client->open_kinds() == std::vector<std::string>{"media"},
                 "open_kinds 含 media");
    report.check(!client->stats_json().empty(), "stats_json 非空");
}

void test_two_channels(TestReport& report) {
    report.section("多通道按 kind 路由");
    const std::string uri = "tcp://127.0.0.1:19711";
    MessageBox server_box, client_box;
    std::string err;
    auto server = NetTunnel::open(echo_server_options(uri, &server_box), &err);
    auto client = NetTunnel::open(dialer_options(uri, &client_box), &err);
    if (!report.check(server && client, "双端 open", err)) return;
    client->wait_connected(5000);

    report.check(client->open_channel("control", true, &err) > 0, "开 Stream 通道 control", err);
    report.check(client->open_channel("media", false, &err) > 0, "开 Datagram 通道 media", err);

    const std::vector<uint8_t> ctrl = {'C'};
    const std::vector<uint8_t> med = {'M', 'M'};
    report.check(try_send(*client, "control", ctrl, 3000), "发 control");
    report.check(try_send(*client, "media", med, 3000), "发 media");

    std::vector<uint8_t> got;
    report.check(server_box.wait_for("control", 3000, &got) && got == ctrl,
                 "服务端按 kind 收到 control");
    report.check(server_box.wait_for("media", 3000, &got) && got == med,
                 "服务端按 kind 收到 media");
}

void test_server_cannot_open(TestReport& report) {
    report.section("服务端开通道被拒绝");
    const std::string uri = "tcp://127.0.0.1:19712";
    MessageBox box;
    std::string err;
    auto server = NetTunnel::open(echo_server_options(uri, &box), &err);
    if (!report.check(server != nullptr, "服务端 open", err)) return;
    const uint32_t ch = server->open_channel("x", false, &err);
    report.check(ch == 0 && err.find("只能接受") != std::string::npos, "返回明确错误", err);
}

void test_reconnect(TestReport& report) {
    report.section("unix 自动重连");
    const std::string uri = "unix:///tmp/kopnet-sdk-reconnect.sock";
    ::unlink(uri.substr(7).c_str());
    MessageBox server_box, client_box;
    LinkState state;
    std::string err;

    auto server = NetTunnel::open(echo_server_options(uri, &server_box), &err);
    if (!report.check(server != nullptr, "服务端 open", err)) return;
    auto client = NetTunnel::open(dialer_options(uri, &client_box, &state), &err);
    if (!report.check(client != nullptr, "拨号端 open", err)) return;
    report.check(client->wait_connected(5000), "首连");
    report.check(state.connected.load(), "on_state(true) 已触发");
    const int baseline_disconnects = state.disconnects.load();
    report.check(client->open_channel("media", false, &err) > 0, "开通道", err);
    const std::vector<uint8_t> payload = {1, 2, 3};
    report.check(try_send(*client, "media", payload, 3000), "首包发送");
    std::vector<uint8_t> got;
    report.check(server_box.wait_for("media", 3000, &got) && got == payload, "服务端收到首包");

    // 销毁服务端：拨号端应感知断开并开始重连
    server->close();
    bool saw_disconnect = false;
    for (int i = 0; i < 300; ++i) {  // 最多 ~6s
        if (state.disconnects.load() > baseline_disconnects) {
            saw_disconnect = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report.check(saw_disconnect, "拨号端感知断开");

    // 同路径重建服务端（unix 监听 bind 时 unlink 旧文件）。
    // 必须先彻底销毁旧服务端：其监听器析构会 unlink socket 文件，
    // 若在新建 bind 之后才析构，会删掉新服务端的文件。
    server.reset();
    server_box.clear();
    server = NetTunnel::open(echo_server_options(uri, &server_box), &err);
    report.check(server != nullptr, "服务端重建", err);

    // 等重连完成并恢复逻辑通道
    bool recovered = false;
    for (int i = 0; i < 240; ++i) {  // 最多 ~12s（覆盖退避）
        if (state.connected.load() && try_send(*client, "media", payload, 150)) {
            std::vector<uint8_t> g;
            if (server_box.wait_for("media", 300, &g) && g == payload) {
                recovered = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    report.check(recovered, "重连后同一逻辑通道恢复通信");
    report.check(client->connected(), "connected() 恢复为真");
}

void test_no_reconnect(TestReport& report) {
    report.section("关闭自动重连即断即止");
    const std::string uri = "unix:///tmp/kopnet-sdk-noreconnect.sock";
    ::unlink(uri.substr(7).c_str());
    MessageBox server_box, client_box;
    LinkState state;
    std::string err;
    auto server = NetTunnel::open(echo_server_options(uri, &server_box), &err);
    if (!report.check(server != nullptr, "服务端 open", err)) return;
    auto client = NetTunnel::open(
        dialer_options(uri, &client_box, &state, /*auto_reconnect=*/false), &err);
    if (!report.check(client != nullptr, "拨号端 open", err)) return;
    report.check(client->wait_connected(5000), "首连");
    report.check(client->open_channel("media", false, &err) > 0, "开通道", err);

    server->close();
    bool saw_disconnect = false;
    for (int i = 0; i < 300; ++i) {
        if (state.disconnects.load() > 0) {
            saw_disconnect = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report.check(saw_disconnect, "感知断开");
    // 断开后确认不再自动重连：连发几次都应失败
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    report.check(!state.connected.load(), "未自动重连");
    std::string send_err;
    const bool ok = client->send("media", "x", 1, &send_err);
    report.check(!ok, "断线后发送失败（不重连）", send_err);
}

void test_close_channel(TestReport& report) {
    report.section("关闭通道");
    const std::string uri = "tcp://127.0.0.1:19713";
    MessageBox server_box, client_box;
    std::string err;
    auto server = NetTunnel::open(echo_server_options(uri, &server_box), &err);
    auto client = NetTunnel::open(dialer_options(uri, &client_box), &err);
    if (!report.check(server && client, "双端 open", err)) return;
    client->wait_connected(5000);
    report.check(client->open_channel("media", false, &err) > 0, "开通道", err);
    client->close_channel("media");
    std::string send_err;
    const bool ok = client->send("media", "x", 1, &send_err);
    report.check(!ok && send_err.find("未打开") != std::string::npos,
                 "关闭后发送得到“未打开”错误", send_err);
}

}  // namespace

int main() {
    TestReport report("KOPNET_SDK");
    test_echo(report);
    test_two_channels(report);
    test_server_cannot_open(report);
    test_reconnect(report);
    test_no_reconnect(report);
    test_close_channel(report);
    report.summary();
    return report.failures();
}
