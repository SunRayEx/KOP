// KOPNET RTP 适配器测试：
//   1. rtp:// 回环（监听 + 拨号）：负载有序到达、首包不丢、双向通信、stats
//   2. 序号丢包/重复统计（手搓 RTP 包注入间隙）
//   3. RTCP 报文复用到同一端口时被识别并跳过
//   4. 限制：MTU 上限拒绝、fd 拒绝、流式语义拒绝
//   5. URI 适配器入口 + 隧道透明性：TunnelSession 骑在 rtp:// 上双向收发
//
// 注意：服务端会话线程的生命周期必须比 test 函数长（用 shared_ptr 保活 +
// 显式 join），否则 run() 在已释放对象上自旋是 use-after-free。
#include "../protocol/rtp_transport.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kopnet/adapters.hpp"
#include "kopnet/endpoint.hpp"
#include "kopnet/tunnel.hpp"

using namespace kopnet;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

static IoStatus drain(RtpTransport* t, std::vector<uint8_t>* out, int timeout_ms) {
    std::string err;
    const IoStatus st = t->wait_readable(timeout_ms, &err);
    if (st != IoStatus::Ok) return st;
    return t->recv_datagram(out, nullptr, &err);
}

// ---- 1. 回环：双向通信 + 负载完整 + 首包保留 ----
static void test_loopback() {
    std::string err;
    const uint16_t port = 38231;
    RtpConfig cfg;
    cfg.payload_type = 100;
    cfg.ssrc = 0xCAFEBABE;

    auto listener = rtp_listen("127.0.0.1", port, cfg, &err);
    check(listener && listener->valid(), "rtp listen");
    if (!listener) return;

    auto client = rtp_connect("127.0.0.1", port, cfg, &err);
    check(client && client->valid(), "rtp connect");
    if (!client) return;
    RtpTransport* rtp_client = dynamic_cast<RtpTransport*>(client.get());
    check(rtp_client != nullptr, "connect 产出 RtpTransport");
    if (!rtp_client) return;

    // 首包必须被 accept 捕获并在首个 recv_datagram 投递
    std::vector<uint8_t> first = {0xde, 0xad, 0xbe, 0xef};
    IoStatus st = client->send_datagram(first.data(), first.size(), nullptr, &err);
    check(st == IoStatus::Ok, "rtp client send first");

    std::unique_ptr<Transport> session;
    st = listener->accept(&session, 3000, &err);
    check(st == IoStatus::Ok && session && session->valid(), "rtp accept session");
    if (!session) return;
    RtpTransport* server = dynamic_cast<RtpTransport*>(session.get());
    check(server != nullptr, "accept 产出 RtpTransport");
    if (!server) return;

    std::vector<uint8_t> got;
    st = server->recv_datagram(&got, nullptr, &err);
    check(st == IoStatus::Ok && got == first, "rtp 首包负载完整到达");

    // 多包负载有序、序号连续无丢失
    bool ordered = true;
    for (uint32_t i = 0; i < 16; ++i) {
        std::vector<uint8_t> p(32, static_cast<uint8_t>(i));
        if (client->send_datagram(p.data(), p.size(), nullptr, &err) != IoStatus::Ok)
            ordered = false;
    }
    for (uint32_t i = 0; i < 16 && ordered; ++i) {
        std::vector<uint8_t> p;
        st = drain(server, &p, 3000);
        if (st != IoStatus::Ok || p.size() != 32 ||
            p.front() != static_cast<uint8_t>(i) || p.back() != static_cast<uint8_t>(i)) {
            ordered = false;
        }
    }
    check(ordered, "rtp 16 包有序到达且负载一致");

    const RtpStats stats = server->stats();
    check(stats.received == 17, "rtp 接收计数 17");
    check(stats.lost == 0, "rtp 无丢包");
    check(stats.duplicates == 0, "rtp 无重复");
    check(stats.rtcp_skipped == 0, "rtp 无 RTCP");
    check(rtp_client->stats().sent == 17, "rtp 发送计数 17");
    check(rtp_client->local_ssrc() == 0xCAFEBABE, "rtp 配置 SSRC 生效");
    check(server->peer_ssrc() == 0xCAFEBABE, "rtp 对端 SSRC 已锁定");

    // 反向：server → client
    std::vector<uint8_t> reply = {1, 2, 3};
    st = server->send_datagram(reply.data(), reply.size(), nullptr, &err);
    check(st == IoStatus::Ok, "rtp server send");
    std::vector<uint8_t> got_reply;
    st = drain(rtp_client, &got_reply, 3000);
    check(st == IoStatus::Ok && got_reply == reply, "rtp 反向到达 client");

    // 发送侧序号确实自增
    const uint16_t s0 = rtp_client->next_send_sequence();
    rtp_client->send_datagram(first.data(), first.size(), nullptr, &err);
    check(rtp_client->next_send_sequence() == static_cast<uint16_t>(s0 + 1), "rtp 序号自增");
}

// ---- 2. 序号丢包与重复统计 ----
static void test_loss_and_duplicates() {
    std::string err;
    const uint16_t port = 38232;
    RtpConfig cfg;
    auto listener = rtp_listen("127.0.0.1", port, cfg, &err);
    check(listener && listener->valid(), "rtp(loss) listen");
    if (!listener) return;

    // 裸 UDP socket 直接注入手搓 RTP 包，制造 [1,2,5,2,6] 的序列
    const int raw = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(raw >= 0, "raw udp socket");
    if (raw < 0) return;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    const auto send_raw = [&](uint16_t seq) {
        std::vector<uint8_t> p(16);
        p[0] = 0x80;
        p[1] = 96;
        p[2] = static_cast<uint8_t>((seq >> 8) & 0xff);
        p[3] = static_cast<uint8_t>(seq & 0xff);
        return ::sendto(raw, p.data(), p.size(), 0, reinterpret_cast<sockaddr*>(&dst),
                        sizeof(dst)) > 0;
    };
    check(send_raw(1), "inject seq=1");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::unique_ptr<Transport> session;
    IoStatus st = listener->accept(&session, 3000, &err);
    check(st == IoStatus::Ok && session, "rtp(loss) accept");
    if (!session) {
        ::close(raw);
        return;
    }
    RtpTransport* server = dynamic_cast<RtpTransport*>(session.get());
    // 吞掉首包（seq=1），锁定基准 expected=2
    std::vector<uint8_t> got;
    server->recv_datagram(&got, nullptr, &err);

    check(send_raw(2), "inject seq=2");
    check(send_raw(5), "inject seq=5（跳过 3,4）");
    check(send_raw(2), "inject seq=2（重复）");
    check(send_raw(6), "inject seq=6");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<uint8_t> buf;
    for (int i = 0; i < 4; ++i) {
        if (drain(server, &buf, 1000) != IoStatus::Ok) break;
    }
    // 首包已吞，后续 2/5/2/6 全部投递 → received=5，lost=2（seq 3,4），duplicates=1
    const RtpStats stats = server->stats();
    check(stats.received == 5, "rtp(loss) 共收到 5 个 RTP 包");
    check(stats.lost == 2, "rtp(loss) 丢失 2（seq 3,4）");
    check(stats.duplicates == 1, "rtp(loss) 重复 1（seq 2）");
    ::close(raw);
}

// ---- 3. RTCP 复用识别并跳过 ----
static void test_rtcp_skip() {
    std::string err;
    const uint16_t port = 38233;
    RtpConfig cfg;
    auto listener = rtp_listen("127.0.0.1", port, cfg, &err);
    check(listener && listener->valid(), "rtp(rtcp) listen");
    if (!listener) return;

    const int raw = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check(raw >= 0, "raw udp socket for rtcp");
    if (raw < 0) return;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    // RTCP SR：V=2 P=0 RC=0 PT=200，length=6 字（首部后 28 字节）
    uint8_t sr[36] = {0};
    sr[0] = 0x80;
    sr[1] = 200;
    sr[3] = 6;
    const ssize_t n = ::sendto(raw, sr, sizeof(sr), 0, reinterpret_cast<sockaddr*>(&dst),
                               sizeof(dst));
    check(n > 0, "inject rtcp sr");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::unique_ptr<Transport> session;
    IoStatus st = listener->accept(&session, 3000, &err);
    check(st == IoStatus::Ok && session, "rtp(rtcp) accept by rtcp packet");
    if (!session) {
        ::close(raw);
        return;
    }
    RtpTransport* server = dynamic_cast<RtpTransport*>(session.get());
    // 投递 pending 的 RTCP 包：被识别为 RTCP，计入 stats 且不作为数据返回
    std::vector<uint8_t> buf;
    st = server->recv_datagram(&buf, nullptr, &err);
    check(st == IoStatus::WouldBlock && buf.empty(), "rtcp 不作为数据投递");
    check(server->stats().rtcp_skipped == 1, "rtcp 被识别并跳过");

    // 随后一个真正的 RTP 包仍能正常收到
    uint8_t rtp[16] = {0};
    rtp[0] = 0x80;
    rtp[1] = 96;
    rtp[3] = 1;  // seq=1
    ::sendto(raw, rtp, sizeof(rtp), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    std::vector<uint8_t> got;
    st = drain(server, &got, 2000);
    check(st == IoStatus::Ok && got.size() == 4, "rtcp 之后的 RTP 包正常投递");
    ::close(raw);
}

// ---- 4. 限制 ----
static void test_limits() {
    std::string err;
    const uint16_t port = 38234;
    RtpConfig cfg;
    auto client = rtp_connect("127.0.0.1", port, cfg, &err);
    check(client && client->valid(), "rtp(limits) connect");
    if (!client) return;

    // 超出数据报上限：拒绝
    std::vector<uint8_t> big(KOPNET_MAX_DATAGRAM);
    IoStatus st = client->send_datagram(big.data(), big.size(), nullptr, &err);
    check(st == IoStatus::Error, "超过上限的负载被拒绝");

    // fd 透传：拒绝
    std::vector<int> fds{0};
    std::vector<uint8_t> small = {1, 2};
    st = client->send_datagram(small.data(), small.size(), &fds, &err);
    check(st == IoStatus::Error, "rtp 拒绝透传 fd");

    // 流式语义：拒绝
    size_t n = 0;
    st = client->read(small.data(), small.size(), &n, nullptr, &err);
    check(st == IoStatus::Error, "rtp 拒绝流式 read");
    st = client->write(small.data(), small.size(), &n, nullptr, &err);
    check(st == IoStatus::Error, "rtp 拒绝流式 write");
}

// ---- 5. URI 适配器入口 + 隧道透明性 ----
static void test_uri_adapter() {
    std::string err;
    const uint16_t port = 38235;
    // shared_ptr：run 线程与测试作用域共同保活，避免 run() 在已释放对象上自旋
    std::shared_ptr<TunnelServer> server = std::make_shared<TunnelServer>();
    check(server->listen("rtp://127.0.0.1:" + std::to_string(port) + "?pt=100", &err),
          "rtp uri serve");

    std::mutex m;
    std::vector<std::shared_ptr<TunnelSession>> sessions;
    std::thread th([server, &m, &sessions] {
        // 处理器立即返回（不阻塞 accept 循环）；会话由 sessions 保活
        server->run([&m, &sessions](std::unique_ptr<TunnelSession> s) {
            std::shared_ptr<TunnelSession> sp(std::move(s));
            TunnelSession* raw = sp.get();
            raw->set_channel_handler(
                [raw](uint32_t id, const std::string&, ChannelMode) {
                    raw->set_data_handler(
                        id, [raw, id](uint32_t, const std::vector<uint8_t>& data,
                                      const std::vector<int>&) {
                            std::vector<int> no_fds;
                            raw->send(id, data.data(), data.size(), &no_fds, 1000);
                        });
                });
            std::lock_guard<std::mutex> lk(m);
            sessions.push_back(std::move(sp));
        });
    });

    TunnelSession::Options opts;
    opts.handshake_ms = 4000;
    auto session = tunnel_dial("rtp://127.0.0.1:" + std::to_string(port) + "?pt=100",
                               opts, &err);
    check(session != nullptr, "rtp uri tunnel_dial 建立会话");
    if (!session) {
        server->stop();
        th.join();
        return;
    }
    check(session->wait_hello(4000, &err), "rtp uri HELLO 协商成功");

    const uint32_t ch =
        session->open_channel("echo", ChannelMode::Datagram, 4000, &err);
    check(ch > 0, "rtp 隧道打开数据报通道");
    if (ch == 0) {
        session->stop();
        server->stop();
        th.join();
        return;
    }

    const std::vector<uint8_t> msg = {'R', 'T', 'P', '-', 'O', 'K'};
    const SendStatus sst = session->send(ch, msg.data(), msg.size(), nullptr, 3000);
    check(sst == SendStatus::Ok, "rtp 隧道发送");

    std::vector<uint8_t> reply;
    std::vector<int> recv_fds;
    const RecvStatus rst = session->recv(ch, &reply, &recv_fds, 3000);
    check(rst == RecvStatus::Ok && reply == msg, "rtp 隧道回显数据一致");

    session->stop();
    server->stop();
    th.join();  // run 线程退出后不再访问 sessions/m
    {
        std::lock_guard<std::mutex> lk(m);
        sessions.clear();  // 销毁服务端会话（停机 + join 各自工作线程）
    }
}

int main() {
    test_loopback();
    test_loss_and_duplicates();
    test_rtcp_skip();
    test_limits();
    test_uri_adapter();
    if (failures == 0) {
        std::fprintf(stderr, "KOPNET rtp tests: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "KOPNET rtp tests: %d FAILURES\n", failures);
    return 1;
}
