#include "kop/sdk/net_tunnel.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "kop/log.h"

namespace kop {
namespace sdk {

namespace {

const char* kTag = "kop-sdk-net";

const char* send_status_text(kopnet::SendStatus st) {
    switch (st) {
        case kopnet::SendStatus::Closed:
            return "通道已关闭";
        case kopnet::SendStatus::Timeout:
            return "发送超时（回压）";
        case kopnet::SendStatus::Error:
            return "发送错误";
        default:
            return "ok";
    }
}

}  // namespace

struct NetTunnel::Impl {
    enum class Role { Dialer, Server } role = Role::Dialer;

    NetTunnelOptions options;
    std::atomic<bool> closed{false};

    // 拨号端：auto_reconnect 用 resilient，否则用一次性会话
    std::unique_ptr<kopnet::ResilientSession> resilient;
    std::unique_ptr<kopnet::TunnelSession> plain;
    std::thread state_thread;  // 一次性拨号端的状态通知

    // 服务端
    std::unique_ptr<kopnet::TunnelServer> server;
    std::thread run_thread;
    std::future<void> run_finished;  // run() 返回时置位，用于 close() 确认循环已退出
    std::shared_ptr<kopnet::TunnelSession> current;  // 仅在 run 线程与 send 时持引用

    // 共享状态
    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, uint32_t> kinds;  // kind -> 通道号
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> frames_received{0};

    void log(const std::string& msg) const {
        if (options.on_log) {
            options.on_log(msg);
        } else {
            KOP_LOG_INFO(kTag, "%s", msg.c_str());
        }
    }

    // 工作线程上投递数据回调
    void dispatch(const std::string& kind, const uint8_t* data, size_t len) {
        frames_received.fetch_add(1, std::memory_order_relaxed);
        if (options.on_data) options.on_data(kind, data, len);
    }

    // 通道 data handler：按 kind 路由到 on_data
    kopnet::DataHandler make_data_handler(const std::string& kind) {
        return [this, kind](uint32_t, const std::vector<uint8_t>& data,
                            const std::vector<int>&) {
            dispatch(kind, data.data(), data.size());
        };
    }

    // 对端（或重连后重开）通道落地：记 kind 映射 + 装 data handler + 通知
    template <typename Session>
    void install_channel_handler(Session& session) {
        session->set_channel_handler(
            [this, &session](uint32_t id, const std::string& kind, kopnet::ChannelMode) {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    kinds[kind] = id;
                }
                session->set_data_handler(id, make_data_handler(kind));
                if (options.on_channel) options.on_channel(kind);
            });
    }
};

NetTunnel::~NetTunnel() { close(); }

bool NetTunnel::start_dialer(std::string* error) {
    Impl& impl = *impl_;
    impl.role = Impl::Role::Dialer;
    const int handshake = impl.options.handshake_ms > 0 ? impl.options.handshake_ms : 5000;

    if (impl.options.auto_reconnect) {
        kopnet::ResilientSession::Options ropts;
        ropts.endpoints = {impl.options.uri};
        ropts.base_delay_ms = impl.options.reconnect_base_ms;
        ropts.max_delay_ms = impl.options.reconnect_max_ms;
        ropts.connect_timeout_ms = handshake;
        ropts.tunnel_opts.handshake_ms = handshake;
        impl.resilient = std::make_unique<kopnet::ResilientSession>();
        impl.resilient->set_options(ropts);
        impl.resilient->set_state_handler(
            [&impl](bool connected) {
                if (impl.options.on_state) impl.options.on_state(connected);
            });
        impl.resilient->set_logger(
            [&impl](const std::string& m) { impl.log(m); });
        impl.install_channel_handler(impl.resilient);
        if (!impl.resilient->start(error)) {
            impl.resilient.reset();
            return false;
        }
        return true;
    }

    // 一次性拨号：手动装配，以便在 start() 之前装好 close handler（避免
    // “握手成功即断”的竞态使状态线程永远等不到关闭通知）
    std::unique_ptr<kopnet::Transport> transport;
    if (!kopnet::AdapterRegistry::instance().dial(impl.options.uri, &transport, error)) {
        return false;
    }
    kopnet::TunnelSession::Options topts;
    topts.handshake_ms = handshake;
    impl.plain = kopnet::TunnelSession::create(std::move(transport), true, topts, error);
    if (!impl.plain) return false;

    auto closed = std::make_shared<std::promise<void>>();
    std::shared_future<void> closed_future = closed->get_future();
    impl.plain->set_close_handler([closed] { closed->set_value(); });
    impl.plain->set_logger([&impl](const std::string& m) { impl.log(m); });
    impl.install_channel_handler(impl.plain);
    if (!impl.plain->start(error)) {
        impl.plain.reset();
        return false;
    }

    // 状态通知：HELLO 成功 → on_state(true)；会话结束 → on_state(false)
    impl.state_thread = std::thread([this, closed_future]() {
        Impl& i = *impl_;
        if (!i.plain) return;
        std::string err;
        if (!i.plain->wait_hello(i.options.handshake_ms > 0 ? i.options.handshake_ms : 5000,
                                 &err)) {
            if (i.options.on_state) i.options.on_state(false);
            return;
        }
        if (i.options.on_state) i.options.on_state(true);
        closed_future.get();
        if (i.options.on_state) i.options.on_state(false);
    });
    return true;
}

bool NetTunnel::start_server(std::string* error) {
    Impl& impl = *impl_;
    impl.role = Impl::Role::Server;
    auto finished = std::make_shared<std::promise<void>>();
    impl.run_finished = finished->get_future();
    impl.server = std::make_unique<kopnet::TunnelServer>();
    if (!impl.server->listen(impl.options.uri, error)) {
        impl.server.reset();
        return false;
    }
    impl.run_thread = std::thread([this, finished]() {
        Impl& i = *impl_;
        i.server->run([this](std::unique_ptr<kopnet::TunnelSession> session) {
            Impl& i = *impl_;
            // 每条接入连接：装 handler 后阻塞到会话结束，返回时由 run() 销毁
            // 会话（在 run 线程析构，不在 worker 线程，避免自 join 死锁）
            auto closed = std::make_shared<std::promise<void>>();
            std::shared_future<void> closed_future = closed->get_future();
            std::shared_ptr<kopnet::TunnelSession> sp(std::move(session));
            kopnet::TunnelSession* raw = sp.get();
            {
                std::lock_guard<std::mutex> lk(i.mu);
                i.current = sp;
                i.kinds.clear();
                i.cv.notify_all();
            }
            raw->set_channel_handler(
                [this, raw](uint32_t id, const std::string& kind, kopnet::ChannelMode) {
                    {
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        impl_->kinds[kind] = id;
                    }
                    raw->set_data_handler(
                        id, [this, kind](uint32_t, const std::vector<uint8_t>& data,
                                         const std::vector<int>&) {
                            impl_->dispatch(kind, data.data(), data.size());
                        });
                    if (impl_->options.on_channel) impl_->options.on_channel(kind);
                });
            raw->set_close_handler([closed] { closed->set_value(); });
            raw->set_logger([this](const std::string& m) { impl_->log(m); });
            if (i.options.on_state) i.options.on_state(true);
            closed_future.get();
            {
                std::lock_guard<std::mutex> lk(i.mu);
                if (i.current == sp) i.current.reset();
                i.kinds.clear();
            }
        });
        finished->set_value();
    });
    return true;
}

std::unique_ptr<NetTunnel> NetTunnel::open(const NetTunnelOptions& options,
                                          std::string* error) {
    if (options.uri.empty()) {
        if (error) *error = "uri 为空";
        return nullptr;
    }
    auto link = std::unique_ptr<NetTunnel>(new NetTunnel());
    link->impl_ = std::make_unique<Impl>();
    link->impl_->options = options;
    const bool ok =
        options.serve ? link->start_server(error) : link->start_dialer(error);
    if (!ok) return nullptr;
    return link;
}

uint32_t NetTunnel::open_channel(const std::string& kind, bool ordered,
                                 std::string* error) {
    Impl& impl = *impl_;
    if (impl.closed.load()) {
        if (error) *error = "tunnel 已关闭";
        return 0;
    }
    if (impl.role == Impl::Role::Server) {
        if (error) *error = "服务端只能接受对端打开的通道";
        return 0;
    }
    const kopnet::ChannelMode mode = ordered ? kopnet::ChannelMode::Stream
                                             : kopnet::ChannelMode::Datagram;
    const int timeout = impl.options.handshake_ms > 0 ? impl.options.handshake_ms : 5000;
    uint32_t id = 0;
    if (impl.resilient) {
        id = impl.resilient->open_channel(kind, mode, timeout, impl.make_data_handler(kind),
                                          error);
    } else if (impl.plain) {
        id = impl.plain->open_channel(kind, mode, timeout, impl.make_data_handler(kind),
                                      error);
    } else {
        if (error) *error = "tunnel 未启动";
        return 0;
    }
    if (id == 0) return 0;
    {
        std::lock_guard<std::mutex> lk(impl.mu);
        impl.kinds[kind] = id;
    }
    return id;
}

bool NetTunnel::send(const std::string& kind, const void* data, size_t len,
                     std::string* error) {
    Impl& impl = *impl_;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lk(impl.mu);
        const auto it = impl.kinds.find(kind);
        if (it == impl.kinds.end()) {
            if (error) *error = "通道 '" + kind + "' 未打开";
            return false;
        }
        id = it->second;
    }
    return send(id, data, len, error);
}

bool NetTunnel::send(uint32_t channel, const void* data, size_t len,
                     std::string* error) {
    Impl& impl = *impl_;
    if (data == nullptr || len == 0) {
        if (error) *error = "空负载";
        return false;
    }
    const auto* ptr = static_cast<const uint8_t*>(data);
    constexpr int kSendTimeoutMs = 2000;
    kopnet::SendStatus st = kopnet::SendStatus::Error;

    if (impl.resilient) {
        st = impl.resilient->send(channel, ptr, len, nullptr, kSendTimeoutMs);
    } else if (impl.plain) {
        st = impl.plain->send(channel, ptr, len, nullptr, kSendTimeoutMs);
    } else if (impl.role == Impl::Role::Server) {
        std::shared_ptr<kopnet::TunnelSession> cur;
        {
            std::lock_guard<std::mutex> lk(impl.mu);
            cur = impl.current;
        }
        if (!cur) {
            if (error) *error = "尚无已连接的会话";
            return false;
        }
        st = cur->send(channel, ptr, len, nullptr, kSendTimeoutMs);
    } else {
        if (error) *error = "tunnel 未启动";
        return false;
    }

    if (st != kopnet::SendStatus::Ok) {
        if (error) *error = send_status_text(st);
        return false;
    }
    impl.frames_sent.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void NetTunnel::close_channel(const std::string& kind) {
    Impl& impl = *impl_;
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lk(impl.mu);
        const auto it = impl.kinds.find(kind);
        if (it == impl.kinds.end()) return;
        id = it->second;
        impl.kinds.erase(it);
    }
    close_channel(id);
}

void NetTunnel::close_channel(uint32_t channel) {
    Impl& impl = *impl_;
    if (channel == 0) return;
    if (impl.resilient) {
        impl.resilient->close_channel(channel);
    } else if (impl.plain) {
        impl.plain->close_channel(channel);
    } else if (impl.role == Impl::Role::Server) {
        std::shared_ptr<kopnet::TunnelSession> cur;
        {
            std::lock_guard<std::mutex> lk(impl.mu);
            cur = impl.current;
        }
        if (cur) cur->close_channel(channel);
    }
    {
        std::lock_guard<std::mutex> lk(impl.mu);
        for (auto it = impl.kinds.begin(); it != impl.kinds.end(); ++it) {
            if (it->second == channel) {
                impl.kinds.erase(it);
                break;
            }
        }
    }
}

bool NetTunnel::wait_connected(int timeout_ms) {
    Impl& impl = *impl_;
    if (impl.role == Impl::Role::Server) {
        std::unique_lock<std::mutex> lk(impl.mu);
        if (impl.current) return true;
        if (timeout_ms <= 0) return false;
        impl.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [&] { return !!impl.current; });
        return !!impl.current;
    }
    if (impl.resilient) return impl.resilient->wait_connected(timeout_ms);
    if (impl.plain) {
        std::string err;
        return impl.plain->wait_hello(timeout_ms, &err);
    }
    return false;
}

bool NetTunnel::connected() const {
    Impl& impl = *impl_;
    if (impl.role == Impl::Role::Server) {
        std::lock_guard<std::mutex> lk(impl.mu);
        return !!impl.current;
    }
    if (impl.resilient) return impl.resilient->connected();
    if (impl.plain) return impl.plain->alive();
    return false;
}

std::vector<std::string> NetTunnel::open_kinds() const {
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        out.reserve(impl_->kinds.size());
        for (const auto& kv : impl_->kinds) out.push_back(kv.first);
    }
    return out;
}

NetTunnelStats NetTunnel::stats() const {
    NetTunnelStats s;
    s.frames_sent = impl_->frames_sent.load(std::memory_order_relaxed);
    s.frames_received = impl_->frames_received.load(std::memory_order_relaxed);
    s.connected = connected();
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        s.channels = impl_->kinds.size();
    }
    return s;
}

std::string NetTunnel::stats_json() const {
    const NetTunnelStats s = stats();
    std::ostringstream os;
    os << "{\"connected\":" << (s.connected ? "true" : "false") << ",\"channels\":"
       << s.channels << ",\"frames_sent\":" << s.frames_sent
       << ",\"frames_received\":" << s.frames_received << "}";
    return os.str();
}

void NetTunnel::close() {
    Impl* impl = impl_.get();
    if (!impl || impl->closed.exchange(true)) return;

    if (impl->role == Impl::Role::Server) {
        // 取走当前会话引用：由本线程释放，确保不在 worker 线程析构（自 join 死锁）
        std::shared_ptr<kopnet::TunnelSession> cur;
        {
            std::lock_guard<std::mutex> lk(impl->mu);
            cur = std::move(impl->current);
        }
        if (cur) cur->stop();  // 触发 close handler → run 线程的 future 返回
        impl->server->stop();
        // TunnelServer::run() 入口会把 stop_ 复位（支持同一服务端重复 run），
        // 若 open() 后立即 close()，首发 stop() 可能被抹掉；用有限重试补发，
        // 保证 run 循环必然退出后再 join。
        if (impl->run_finished.valid()) {
            for (int k = 0; k < 40; ++k) {  // 最多约 10s
                if (impl->run_finished.wait_for(std::chrono::milliseconds(250)) ==
                    std::future_status::ready) {
                    break;
                }
                impl->server->stop();
            }
        }
        if (impl->run_thread.joinable()) impl->run_thread.join();
        cur.reset();  // run 线程已退出，此处释放安全
        {
            std::lock_guard<std::mutex> lk(impl->mu);
            impl->kinds.clear();
        }
        return;
    }

    if (impl->resilient) impl->resilient->stop();
    if (impl->plain) impl->plain->stop();
    if (impl->state_thread.joinable()) impl->state_thread.join();
    impl->resilient.reset();
    impl->plain.reset();
}

}  // namespace sdk
}  // namespace kop
