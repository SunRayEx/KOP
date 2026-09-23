// kopnet-relay：KOPNET 中继。
//
// 两种角色：
//   1) 承载角色（默认）：在 --listen 端点上 Serve，把每条接入隧道的每个
//      通道按 kind 转发到 --target 端点的一条新隧道（或本地处理）；
//   2) SSH 伙伴角色（--stdio）：把自身的 stdin/stdout 作为隧道承载，
//      供 `ssh host kopnet-relay --stdio` 直接把远端 KOPNET 接回来。
//
// 当前实现：最小可用中继——把接入会话的通道原样桥接到目标端点
// （每个 kind 一个通道，数据双向透传），并把生命周期日志打到 stderr。
//
// 用法：
//   kopnet-relay --listen tcp://0.0.0.0:7700 --target unix:kopms-session
//   ssh -T host kopnet-relay --stdio --target unix:kopms-session
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include "kop/log.h"
#include "kopnet/adapters.hpp"
#include "kopnet/tunnel.hpp"

namespace {

static const char* kTag = "kopnet-relay";

struct Args {
    std::string listen_uri;
    std::string target_uri;
    bool stdio = false;
    int verbose = 1;
};

bool parse_args(int argc, char** argv, Args* out) {
    static const struct option opts[] = {
        {"listen", required_argument, nullptr, 'l'},
        {"target", required_argument, nullptr, 't'},
        {"stdio", no_argument, nullptr, 's'},
        {"quiet", no_argument, nullptr, 'q'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}};
    int c = 0;
    while ((c = ::getopt_long(argc, argv, "l:t:sqh", opts, nullptr)) != -1) {
        switch (c) {
            case 'l': out->listen_uri = optarg; break;
            case 't': out->target_uri = optarg; break;
            case 's': out->stdio = true; break;
            case 'q': out->verbose = 0; break;
            case 'h':
                std::printf(
                    "用法: kopnet-relay [--listen URI] [--target URI] [--stdio]\n"
                    "  --listen/-l URI  在该端点 Serve（如 tcp://0.0.0.0:7700）\n"
                    "  --target/-t URI  把接入通道桥接到该端点\n"
                    "  --stdio/-s       以 stdin/stdout 为承载（SSH 伙伴角色）\n");
                return false;
            default: return false;
        }
    }
    return true;
}

// 一条接入通道 ↔ 目标通道的双向桥
struct Bridge {
    std::shared_ptr<kopnet::TunnelSession> up;      // 接入侧
    std::shared_ptr<kopnet::TunnelSession> down;    // 目标侧
    uint32_t up_channel = 0;
    uint32_t down_channel = 0;

    void pump_up_to_down() {
        std::vector<uint8_t> data;
        std::vector<int> fds;
        while (up->alive()) {
            if (up->recv(up_channel, &data, &fds, 1000) != kopnet::RecvStatus::Ok) continue;
            down->send(down_channel, data.data(), data.size(),
                       fds.empty() ? nullptr : &fds, 5000);
        }
    }
    void pump_down_to_up() {
        std::vector<uint8_t> data;
        std::vector<int> fds;
        while (up->alive()) {
            if (down->recv(down_channel, &data, &fds, 1000) != kopnet::RecvStatus::Ok) continue;
            up->send(up_channel, data.data(), data.size(),
                     fds.empty() ? nullptr : &fds, 5000);
        }
    }
};

std::shared_ptr<kopnet::TunnelSession> dial_target(const std::string& uri,
                                                   std::string* error) {
    kopnet::TunnelSession::Options opts;
    return kopnet::tunnel_dial(uri, opts, error);
}

void bridge_session(std::shared_ptr<kopnet::TunnelSession> up,
                       const std::string& target_uri) {
    // 接入侧每开一个通道，就在目标侧开同 kind 通道并双向泵
    up->set_channel_handler([up, target_uri](uint32_t id, const std::string& kind,
                                             kopnet::ChannelMode mode) {
        std::string error;
        auto down = dial_target(target_uri, &error);
        if (!down) {
            KOP_LOG_ERROR(kTag, "无法连接目标 %s: %s", target_uri.c_str(), error.c_str());
            up->close_channel(id);
            return;
        }
        uint32_t down_id = down->open_channel(kind, mode, 5000, &error);
        if (down_id == 0) {
            KOP_LOG_ERROR(kTag, "打开目标通道失败: %s", error.c_str());
            up->close_channel(id);
            return;
        }
        auto b = std::make_shared<Bridge>();
        b->up = up;
        b->down = std::move(down);
        b->up_channel = id;
        b->down_channel = down_id;
        KOP_LOG_INFO(kTag, "桥接通道 %s（本地 %u ↔ 远端 %u）", kind.c_str(), id, down_id);
        std::thread([b] { b->pump_up_to_down(); }).detach();
        std::thread([b] { b->pump_down_to_up(); }).detach();
    });
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) return 1;
    if (args.target_uri.empty()) {
        std::fprintf(stderr, "必须指定 --target\n");
        return 1;
    }

    if (args.stdio) {
        // SSH 伙伴角色：stdin/stdout 即承载通路
        std::string error;
        auto transport = kopnet::make_stdio_transport(&error);
        if (!transport) {
            std::fprintf(stderr, "stdio 传输创建失败: %s\n", error.c_str());
            return 1;
        }
        kopnet::TunnelSession::Options opts;
        auto session = kopnet::TunnelSession::create(std::move(transport), /*is_dialer=*/false,
                                                     opts, &error);
        if (!session || !session->start(&error)) {
            std::fprintf(stderr, "隧道协商失败: %s\n", error.c_str());
            return 1;
        }
        KOP_LOG_INFO(kTag, "stdio 隧道已建立 → 目标 %s", args.target_uri.c_str());
        auto keep = std::shared_ptr<kopnet::TunnelSession>(std::move(session));
        bridge_session(keep, args.target_uri);
        while (keep->alive()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }

    if (args.listen_uri.empty()) {
        std::fprintf(stderr, "必须指定 --listen 或 --stdio\n");
        return 1;
    }

    kopnet::TunnelServer server;
    std::string error;
    if (!server.listen(args.listen_uri, &error)) {
        std::fprintf(stderr, "监听失败: %s\n", error.c_str());
        return 1;
    }
    KOP_LOG_INFO(kTag, "中继监听 %s → 目标 %s", args.listen_uri.c_str(),
                 args.target_uri.c_str());

    std::vector<std::shared_ptr<kopnet::TunnelSession>> sessions;
    std::mutex mu;
    std::atomic<bool> running{true};

    server.run([&](std::unique_ptr<kopnet::TunnelSession> session) {
        auto ptr = std::shared_ptr<kopnet::TunnelSession>(std::move(session));
        {
            std::lock_guard<std::mutex> l(mu);
            sessions.push_back(ptr);
        }
        bridge_session(ptr, args.target_uri);
    });
    (void)running;
    return 0;
}
