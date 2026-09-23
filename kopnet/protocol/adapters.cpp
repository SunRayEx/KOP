// KOPNET 适配器注册表实现。
//
// 内置 adapter：
//   tcp://  udp://  unix://  —— 直接复用 transport/ 内部工厂
//   ssh://  —— spawn `ssh [-p port] [-l user] host remote-command`，stdio 管道承载隧道
//   relay://name —— AF_UNIX（$XDG_RUNTIME_DIR/kopnet/relay/<name>）连接中继
//   rtp:// rtc:// rdp:// —— 注册点存在但返回“未实现”，供后续里程碑填充
#include "kopnet/adapters.hpp"

#include <memory>
#include <string>
#include <vector>

#include "transport/internal_factories.hpp"
#include "kop/log.h"
#include "kopnet/endpoint.hpp"
#include "rtp_transport.hpp"

namespace kopnet {

namespace {

std::unique_ptr<Transport> ssh_dial(const Endpoint& ep, std::string* error) {
    std::vector<std::string> argv;
    argv.push_back("ssh");
    if (ep.port != 0) {
        argv.push_back("-p");
        argv.push_back(std::to_string(ep.port));
    }
    if (!ep.user.empty()) {
        argv.push_back("-l");
        argv.push_back(ep.user);
    }
    // 关闭伪终端分配、压缩与交互提示，保证 stdio 是干净的二进制通道
    argv.push_back("-T");
    argv.push_back("-o");
    argv.push_back("BatchMode=yes");
    argv.push_back(ep.host);
    if (!ep.path.empty()) argv.push_back(ep.path);
    std::unique_ptr<Transport> t = spawn_pipe_process(argv, error);
    if (!t && error->empty()) *error = "无法启动 ssh 子进程";
    return t;
}

std::string relay_socket_name(const Endpoint& ep) {
    // $XDG_RUNTIME_DIR 相对，沿用 BUS2LAYER 约定
    return "kopnet/relay/" + ep.host;
}

}  // namespace

AdapterRegistry::AdapterRegistry() { register_builtin(); }

AdapterRegistry& AdapterRegistry::instance() {
    static AdapterRegistry registry;
    return registry;
}

bool AdapterRegistry::register_adapter(ProtocolAdapter adapter) {
    for (auto& existing : adapters_) {
        if (existing.scheme == adapter.scheme) {
            existing = std::move(adapter);
            return true;
        }
    }
    adapters_.push_back(std::move(adapter));
    return true;
}

void AdapterRegistry::register_builtin() {
    ProtocolAdapter tcp;
    tcp.scheme = "tcp";
    tcp.dial = [](const Endpoint& ep, std::string* error) {
        return open_raw_transport(ep, error);
    };
    tcp.serve = [](const Endpoint& ep, std::string* error) {
        return create_transport_listener(ep, error);
    };
    adapters_.push_back(std::move(tcp));

    ProtocolAdapter udp;
    udp.scheme = "udp";
    udp.dial = [](const Endpoint& ep, std::string* error) {
        return open_raw_transport(ep, error);
    };
    udp.serve = [](const Endpoint& ep, std::string* error) {
        return create_transport_listener(ep, error);
    };
    adapters_.push_back(std::move(udp));

    ProtocolAdapter unixp;
    unixp.scheme = "unix";
    unixp.dial = [](const Endpoint& ep, std::string* error) {
        return open_raw_transport(ep, error);
    };
    unixp.serve = [](const Endpoint& ep, std::string* error) {
        return create_transport_listener(ep, error);
    };
    adapters_.push_back(std::move(unixp));

    ProtocolAdapter ssh;
    ssh.scheme = "ssh";
    ssh.dial = ssh_dial;
    ssh.serve = nullptr;  // SSH Serve 由远端 sshd + kopnet-relay 承担
    adapters_.push_back(std::move(ssh));

    ProtocolAdapter relay;
    relay.scheme = "relay";
    relay.dial = [](const Endpoint& ep, std::string* error) {
        Endpoint unix_ep;
        unix_ep.scheme = Scheme::Unix;
        unix_ep.host = relay_socket_name(ep);
        return open_raw_transport(unix_ep, error);
    };
    relay.serve = nullptr;  // 中继端用 relay CLI 直接监听
    adapters_.push_back(std::move(relay));

    ProtocolAdapter rtp;
    rtp.scheme = "rtp";
    rtp.dial = [](const Endpoint& ep, std::string* error) {
        RtpConfig config;
        config.payload_type =
            static_cast<uint8_t>(ep.param_int("pt", config.payload_type));
        config.clock_rate =
            static_cast<uint32_t>(ep.param_int("clock", config.clock_rate));
        config.ts_step = static_cast<uint32_t>(ep.param_int("ts_step", 0));
        config.fps = static_cast<uint32_t>(ep.param_int("fps", 30));
        const long ssrc = ep.param_int("ssrc", 0);
        config.ssrc = ssrc > 0 ? static_cast<uint32_t>(ssrc) : 0;
        return rtp_connect(ep.host, ep.port, config, error);
    };
    rtp.serve = [](const Endpoint& ep, std::string* error) {
        RtpConfig config;
        config.payload_type =
            static_cast<uint8_t>(ep.param_int("pt", config.payload_type));
        config.clock_rate =
            static_cast<uint32_t>(ep.param_int("clock", config.clock_rate));
        return rtp_listen(ep.host, ep.port, config, error);
    };
    adapters_.push_back(std::move(rtp));

    auto not_implemented = [](const std::string& name) {
        return [name](const Endpoint&, std::string* error) -> std::unique_ptr<Transport> {
            *error = name + " adapter 尚未实现（需 DTLS-SRTP/ICE 栈，离线不可引入）";
            return nullptr;
        };
    };

    ProtocolAdapter rtc;
    rtc.scheme = "rtc";
    rtc.dial = not_implemented("rtc");
    rtc.serve = nullptr;
    adapters_.push_back(std::move(rtc));

    ProtocolAdapter rdp;
    rdp.scheme = "rdp";
    rdp.dial = not_implemented("rdp");
    rdp.serve = nullptr;
    adapters_.push_back(std::move(rdp));
}

bool AdapterRegistry::dial(const std::string& uri, std::unique_ptr<Transport>* out,
                           std::string* error) const {
    Endpoint ep;
    if (!endpoint_parse(uri, &ep, error)) return false;
    for (const auto& a : adapters_) {
        if (a.scheme == scheme_name(ep.scheme)) {
            if (!a.dial) {
                *error = std::string("scheme [") + a.scheme + "] 不支持 Connect";
                return false;
            }
            auto t = a.dial(ep, error);
            if (!t) return false;
            *out = std::move(t);
            return true;
        }
    }
    *error = std::string("未知 scheme: ") + uri;
    return false;
}

bool AdapterRegistry::serve(const std::string& uri, std::unique_ptr<TransportListener>* out,
                            std::string* error) const {
    Endpoint ep;
    if (!endpoint_parse(uri, &ep, error)) return false;
    for (const auto& a : adapters_) {
        if (a.scheme == scheme_name(ep.scheme)) {
            if (!a.serve) {
                *error = std::string("scheme [") + a.scheme + "] 不支持 Serve";
                return false;
            }
            auto l = a.serve(ep, error);
            if (!l) return false;
            *out = std::move(l);
            return true;
        }
    }
    *error = std::string("未知 scheme: ") + uri;
    return false;
}

bool AdapterRegistry::has_scheme(const std::string& scheme) const {
    for (const auto& a : adapters_) {
        if (a.scheme == scheme) return true;
    }
    return false;
}

std::vector<std::string> AdapterRegistry::schemes() const {
    std::vector<std::string> out;
    out.reserve(adapters_.size());
    for (const auto& a : adapters_) out.push_back(a.scheme);
    return out;
}

std::unique_ptr<TunnelSession> tunnel_dial(const std::string& uri,
                                           TunnelSession::Options opts, std::string* error) {
    std::unique_ptr<Transport> transport;
    if (!AdapterRegistry::instance().dial(uri, &transport, error)) return nullptr;
    auto session = TunnelSession::create(std::move(transport), /*is_dialer=*/true, opts, error);
    if (!session) return nullptr;
    if (!session->start(error)) return nullptr;
    return session;
}

bool TunnelServer::listen(const std::string& uri, std::string* error) {
    return AdapterRegistry::instance().serve(uri, &listener_, error);
}

void TunnelServer::run(Handler handler) {
    if (!listener_) return;
    stop_.store(false);
    while (!stop_.load()) {
        std::unique_ptr<Transport> transport;
        std::string error;
        IoStatus st = listener_->accept(&transport, 200, &error);
        if (st == IoStatus::WouldBlock) continue;
        if (st != IoStatus::Ok) {
            KOP_LOG_WARN("kopnet", "接受连接失败: %s", error.c_str());
            continue;
        }
        TunnelSession::Options opts;
        std::string start_error;
        auto session = TunnelSession::create(std::move(transport), /*is_dialer=*/false, opts,
                                             &start_error);
        if (!session) {
            KOP_LOG_WARN("kopnet", "创建隧道会话失败: %s", start_error.c_str());
            continue;
        }
        if (!session->start(&start_error)) {
            KOP_LOG_WARN("kopnet", "隧道协商失败: %s", start_error.c_str());
            continue;
        }
        handler(std::move(session));
    }
}

void TunnelServer::stop() { stop_.store(true); }

TunnelServer::~TunnelServer() { stop(); }

}  // namespace kopnet
