// KOPNET 传输层测试：socketpair 回环（含 SCM_RIGHTS fd 透传）、
// TCP 拨号/监听、UDP 会话化监听、resolve_unix_path。
#include "kopnet/endpoint.hpp"
#include "kopnet/transport.hpp"
#include "kopnet/transport_listener.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../transport/internal_factories.hpp"

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

// socketpair 回环 + 字节流分帧循环写满/读回
static void test_socketpair_stream() {
    std::string err;
    std::unique_ptr<Transport> a;
    std::unique_ptr<Transport> b;
    check(make_socketpair_transports(&a, &b, &err), "socketpair create");
    if (!a || !b) return;

    std::vector<uint8_t> out(1 << 16);
    std::vector<uint8_t> want;
    // 写 3 个不同长度块，分多次写/读
    for (size_t len : {8u, 4096u, 60000u}) {
        std::vector<uint8_t> buf(len);
        for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(i & 0xff);
        want.insert(want.end(), buf.begin(), buf.end());
    }
    size_t written = 0;
    while (written < want.size()) {
        size_t n = 0;
        IoStatus st = a->write(want.data() + written, want.size() - written, &n, nullptr,
                               &err);
        if (st == IoStatus::WouldBlock) {
            a->wait_writable(1000, &err);
            continue;
        }
        check(st == IoStatus::Ok, "socketpair write");
        if (st != IoStatus::Ok) return;
        written += n;
    }

    std::vector<uint8_t> got;
    while (got.size() < want.size()) {
        IoStatus rs = b->wait_readable(2000, &err);
        check(rs == IoStatus::Ok, "socketpair wait_readable");
        if (rs != IoStatus::Ok) return;
        size_t n = 0;
        IoStatus st = b->read(out.data(), out.size(), &n, nullptr, &err);
        check(st == IoStatus::Ok, "socketpair read");
        if (st != IoStatus::Ok) return;
        got.insert(got.end(), out.data(), out.data() + n);
    }
    check(got == want, "socketpair byte stream integrity");

    // 关闭一侧，另一侧读到 Closed
    a->close();
    IoStatus rs = b->wait_readable(2000, &err);
    size_t n = 0;
    IoStatus st = b->read(out.data(), out.size(), &n, nullptr, &err);
    check(st == IoStatus::Closed || rs == IoStatus::Closed, "socketpair closed signaled");
}

// SCM_RIGHTS fd 透传
static void test_socketpair_fds() {
    std::string err;
    std::unique_ptr<Transport> a;
    std::unique_ptr<Transport> b;
    if (!make_socketpair_transports(&a, &b, &err)) return;

    // 创建一个 memfd 作为“DMA-BUF 替身”）
    int fd = ::memfd_create("kopnet-test", MFD_CLOEXEC);
    check(fd >= 0, "memfd_create");
    if (fd < 0) return;
    const char* payload = "hello-fd";
    ssize_t w = ::write(fd, payload, 8);
    (void)w;

    std::vector<uint8_t> msg = {1, 2, 3, 4};
    std::vector<int> fds = {fd};
    size_t n = 0;
    IoStatus st = a->write(msg.data(), msg.size(), &n, &fds, &err);
    check(st == IoStatus::Ok && n == msg.size(), "sendmsg with fd");

    std::vector<uint8_t> buf(16);
    std::vector<int> got_fds;
    while (true) {
        b->wait_readable(2000, &err);
        n = 0;
        st = b->read(buf.data(), buf.size(), &n, &got_fds, &err);
        if (st == IoStatus::Ok && n == 0) continue;
        break;
    }
    check(st == IoStatus::Ok && n == 4, "recvmsg got data");
    check(got_fds.size() == 1, "recvmsg got 1 fd");
    if (got_fds.size() == 1) {
        char rbuf[16] = {0};
        ::lseek(got_fds[0], 0, SEEK_SET);
        ssize_t r = ::read(got_fds[0], rbuf, sizeof(rbuf));
        check(r == 8 && std::memcmp(rbuf, payload, 8) == 0, "fd content readable");
        ::close(got_fds[0]);
    }
    ::close(fd);
}

// TCP 拨号/监听（真实环回端口）
static void test_tcp() {
    std::string err;
    const uint16_t port = 38117;
    auto listener = tcp_listen("127.0.0.1", port, &err);
    check(listener && listener->valid(), "tcp listen fixed port");
    if (!listener) return;

    std::thread server([&] {
        std::unique_ptr<Transport> conn;
        std::string e2;
        IoStatus st = listener->accept(&conn, 3000, &e2);
        if (st != IoStatus::Ok) {
            std::fprintf(stderr, "FAIL: tcp accept (%s)\n", e2.c_str());
            ++failures;
            return;
        }
        std::vector<uint8_t> buf(64);
        size_t n = 0;
        conn->wait_readable(3000, &e2);
        st = conn->read(buf.data(), buf.size(), &n, nullptr, &e2);
        if (st != IoStatus::Ok || n != 5) {
            std::fprintf(stderr, "FAIL: tcp read\n");
            ++failures;
            return;
        }
        std::vector<uint8_t> reply = {9, 9, 9};
        size_t w2 = 0;
        conn->write(reply.data(), reply.size(), &w2, nullptr, &e2);
    });

    auto client = tcp_connect("127.0.0.1", port, &err);
    check(client && client->valid(), "tcp connect");
    if (!client) {
        server.join();
        return;
    }
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    size_t n = 0;
    IoStatus st = client->write(msg.data(), msg.size(), &n, nullptr, &err);
    check(st == IoStatus::Ok && n == 5, "tcp write");
    std::vector<uint8_t> buf(16);
    client->wait_readable(3000, &err);
    n = 0;
    st = client->read(buf.data(), buf.size(), &n, nullptr, &err);
    check(st == IoStatus::Ok && n == 3, "tcp read reply");
    server.join();
}

// UDP 会话化监听
static void test_udp() {
    std::string err;
    const uint16_t port = 38129;
    auto listener = udp_listen("127.0.0.1", port, &err);
    check(listener && listener->valid(), "udp listen");
    if (!listener) return;

    auto client = udp_connect("127.0.0.1", port, &err);
    check(client && client->valid(), "udp connect");
    if (!client) return;

    std::vector<uint8_t> msg = {1, 2, 3};
    IoStatus st = client->send_datagram(msg.data(), msg.size(), nullptr, &err);
    check(st == IoStatus::Ok, "udp send");

    std::unique_ptr<Transport> session;
    st = listener->accept(&session, 3000, &err);
    check(st == IoStatus::Ok && session && session->valid(), "udp accept session");
    if (!session) return;
    std::vector<uint8_t> got;
    st = session->recv_datagram(&got, nullptr, &err);
    check(st == IoStatus::Ok && got == msg, "udp first datagram preserved");

    // 会话化后双向通信
    std::vector<uint8_t> reply = {7};
    st = session->send_datagram(reply.data(), reply.size(), nullptr, &err);
    check(st == IoStatus::Ok, "udp session send");
    std::vector<uint8_t> got2;
    st = client->recv_datagram(&got2, nullptr, &err);
    check(st == IoStatus::Ok && got2 == reply, "udp client recv reply");
}

static void test_unix_path() {
    std::string err;
    std::string path;
    ::setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    check(resolve_unix_path("kop-test.sock", &path, &err) && path == "/tmp/kop-test.sock",
          "resolve relative name");
    check(resolve_unix_path("/var/run/kop.sock", &path, &err) &&
              path == "/var/run/kop.sock",
          "resolve absolute path");
}

int main() {
    test_socketpair_stream();
    test_socketpair_fds();
    test_tcp();
    test_udp();
    test_unix_path();
    if (failures == 0) {
        std::fprintf(stderr, "KOPNET transport tests: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "KOPNET transport tests: %d FAILURES\n", failures);
    return 1;
}
