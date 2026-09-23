// KOPNET 传输工厂：按 scheme 创建监听器/原始传输。
#include "internal_factories.hpp"

#include <csignal>
#include <memory>
#include <string>

#include "kopnet/endpoint.hpp"

namespace kopnet {

// 对已关闭读端的管道 / 半连接 socket 写入会触发 SIGPIPE，默认处置是杀死
// 进程。KOPNET 的写错误一律通过 IoStatus::Error + errno 上报，故库入口处
// 统一忽略 SIGPIPE（socket 路径另用 MSG_NOSIGNAL 双保险；管道只能靠它）。
void ignore_sigpipe() {
    static bool installed = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)installed;
}

std::unique_ptr<TransportListener> create_transport_listener(const Endpoint& ep,
                                                             std::string* error) {
    ignore_sigpipe();
    switch (ep.scheme) {
        case Scheme::Tcp: return tcp_listen(ep.host, ep.port, error);
        case Scheme::Unix: return unix_listen(ep.host, error);
        case Scheme::Udp: return udp_listen(ep.host, ep.port, error);
        default:
            *error = std::string("scheme [") + scheme_name(ep.scheme) + "] 不支持监听";
            return nullptr;
    }
}

std::unique_ptr<Transport> open_raw_transport(const Endpoint& ep, std::string* error) {
    ignore_sigpipe();
    switch (ep.scheme) {
        case Scheme::Tcp: return tcp_connect(ep.host, ep.port, error);
        case Scheme::Unix: return unix_connect(ep.host, error);
        case Scheme::Udp: return udp_connect(ep.host, ep.port, error);
        default:
            *error = std::string("scheme [") + scheme_name(ep.scheme) + "] 无原始传输";
            return nullptr;
    }
}

}  // namespace kopnet
