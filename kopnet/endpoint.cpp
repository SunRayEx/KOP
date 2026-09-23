// KOPNET 端点解析实现。
#include "kopnet/endpoint.hpp"

#include <cstdlib>
#include <cstring>

#include "kop/log.h"

namespace kopnet {

namespace {

// 小写化
std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 百分号解码（ssh 远程命令路径可能编码空格等）
std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(in[i + 1]) &&
            std::isxdigit(in[i + 2])) {
            int hi = in[i + 1] >= 'a' ? in[i + 1] - 'a' + 10 : in[i + 1] - '0';
            if (in[i + 1] >= 'A' && in[i + 1] <= 'F') hi = in[i + 1] - 'A' + 10;
            int lo = in[i + 2] >= 'a' ? in[i + 2] - 'a' + 10 : in[i + 2] - '0';
            if (in[i + 2] >= 'A' && in[i + 2] <= 'F') lo = in[i + 2] - 'A' + 10;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// 分离 scheme://rest。成功时 rest 为 authority[+path+query]。
bool split_scheme(const std::string& uri, std::string* scheme, std::string* rest,
                  std::string* error) {
    size_t pos = uri.find("://");
    if (pos == std::string::npos) {
        // 容许 unix:name / relay:name 这类无 // 的简写
        size_t colon = uri.find(':');
        if (colon == std::string::npos) {
            *error = "endpoint URI 缺少 scheme（如 tcp://host:port）";
            return false;
        }
        *scheme = to_lower(uri.substr(0, colon));
        *rest = uri.substr(colon + 1);
        return true;
    }
    *scheme = to_lower(uri.substr(0, pos));
    *rest = uri.substr(pos + 3);
    return true;
}

}  // namespace

std::string Endpoint::param(const std::string& key) const {
    auto it = params.find(to_lower(key));
    return it == params.end() ? std::string() : it->second;
}

long Endpoint::param_int(const std::string& key, long fallback) const {
    std::string v = param(key);
    if (v.empty()) return fallback;
    try {
        return std::stol(v);
    } catch (...) {
        return fallback;
    }
}

const char* scheme_name(Scheme s) {
    switch (s) {
        case Scheme::Tcp: return "tcp";
        case Scheme::Udp: return "udp";
        case Scheme::Unix: return "unix";
        case Scheme::Ssh: return "ssh";
        case Scheme::Rtp: return "rtp";
        case Scheme::Rtc: return "rtc";
        case Scheme::Rdp: return "rdp";
        case Scheme::Relay: return "relay";
        default: return "unknown";
    }
}

TransportSemantics scheme_semantics(Scheme s) {
    switch (s) {
        case Scheme::Tcp:
        case Scheme::Unix:
        case Scheme::Ssh:
        case Scheme::Rdp:
            return TransportSemantics::Stream;
        case Scheme::Udp:
        case Scheme::Rtp:
        case Scheme::Rtc:
            return TransportSemantics::Datagram;
        default:
            return TransportSemantics::Stream;
    }
}

bool scheme_supports_serve(Scheme s) {
    switch (s) {
        case Scheme::Tcp:
        case Scheme::Unix:
        case Scheme::Udp:
        case Scheme::Rtp:
        case Scheme::Rtc:
        case Scheme::Relay:
            return true;
        // ssh/rdp 只能由远端被动建立后由 relay 提供 Serve 语义
        case Scheme::Ssh:
        case Scheme::Rdp:
        default:
            return false;
    }
}

uint16_t scheme_default_port(Scheme s) {
    switch (s) {
        case Scheme::Tcp:
        case Scheme::Udp: return 0;  // 必须显式指定
        case Scheme::Ssh: return 22;
        case Scheme::Rtp:
        case Scheme::Rtc: return 0;  // 必须显式指定
        case Scheme::Rdp: return 3389;
        default: return 0;
    }
}

bool endpoint_parse(const std::string& uri, Endpoint* out, std::string* error) {
    if (uri.empty()) {
        *error = "endpoint URI 为空";
        return false;
    }
    std::string scheme_str;
    std::string rest;
    if (!split_scheme(uri, &scheme_str, &rest, error)) return false;

    Endpoint ep;
    ep.raw = uri;

    if (scheme_str == "tcp") ep.scheme = Scheme::Tcp;
    else if (scheme_str == "udp")
        ep.scheme = Scheme::Udp;
    else if (scheme_str == "unix")
        ep.scheme = Scheme::Unix;
    else if (scheme_str == "ssh")
        ep.scheme = Scheme::Ssh;
    else if (scheme_str == "rtp")
        ep.scheme = Scheme::Rtp;
    else if (scheme_str == "rtc")
        ep.scheme = Scheme::Rtc;
    else if (scheme_str == "rdp")
        ep.scheme = Scheme::Rdp;
    else if (scheme_str == "relay")
        ep.scheme = Scheme::Relay;
    else {
        *error = "未知 scheme: " + scheme_str;
        return false;
    }

    // 分离 query
    std::string authority_path = rest;
    size_t q = rest.find('?');
    if (q != std::string::npos) {
        authority_path = rest.substr(0, q);
        std::string query = rest.substr(q + 1);
        size_t start = 0;
        while (start <= query.size()) {
            size_t amp = query.find('&', start);
            std::string pair = amp == std::string::npos ? query.substr(start)
                                                         : query.substr(start, amp - start);
            if (!pair.empty()) {
                size_t eq = pair.find('=');
                std::string k = eq == std::string::npos ? pair : pair.substr(0, eq);
                std::string v = eq == std::string::npos ? "1" : pair.substr(eq + 1);
                ep.params[to_lower(k)] = v;
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }

    const bool is_unix = ep.scheme == Scheme::Unix;
    const bool is_relay = ep.scheme == Scheme::Relay;

    if (is_unix || is_relay) {
        // unix:name（XDG_RUNTIME_DIR 相对）/ unix:///abs/path / relay:name
        std::string body = authority_path;
        // 去掉形如 "//abs/path" 的前导斜杠对，保留绝对路径语义
        if (!body.empty() && body[0] == '/') {
            // 绝对路径：authoritative body 即路径
            ep.host = body;
        } else {
            ep.host = body;  // 名字（相对 XDG_RUNTIME_DIR）
        }
        if (ep.host.empty()) {
            *error = std::string(scheme_name(ep.scheme)) + " 端点缺少名字或路径";
            return false;
        }
        *out = ep;
        return true;
    }

    // host[:port][/path] 解析（authority 可带 user@）
    std::string authority = authority_path;
    std::string path;
    if (ep.scheme == Scheme::Ssh) {
        size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            path = authority.substr(slash + 1);
            authority = authority.substr(0, slash);
        }
        if (path.empty()) {
            // ssh 无远程命令时使用默认 relay 命令
            path = "kopnet-relay";
        } else {
            path = url_decode(path);
        }
        ep.path = path;
    } else {
        // 非 ssh scheme 不期望 path；出现则当作忽略并记录
        size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            *error = std::string(scheme_name(ep.scheme)) + " 端点不支持路径部分";
            return false;
        }
    }

    size_t at = authority.find('@');
    if (at != std::string::npos) {
        ep.user = authority.substr(0, at);
        authority = authority.substr(at + 1);
        if (ep.user.empty()) {
            *error = "endpoint 的 user@ 部分为空";
            return false;
        }
    }

    // host[:port]（支持 IPv6 的 [::1]:port 形式）
    if (!authority.empty() && authority[0] == '[') {
        size_t rb = authority.find(']');
        if (rb == std::string::npos) {
            *error = "IPv6 端点缺少 ']'";
            return false;
        }
        ep.host = authority.substr(1, rb);
        if (rb + 1 < authority.size()) {
            if (authority[rb + 1] != ':') {
                *error = "IPv6 端点 ':' 位置非法";
                return false;
            }
            std::string port_str = authority.substr(rb + 2);
            long p = 0;
            try {
                p = std::stol(port_str);
            } catch (...) {
                *error = "端口号非法: " + port_str;
                return false;
            }
            if (p <= 0 || p > 65535) {
                *error = "端口号超出范围: " + port_str;
                return false;
            }
            ep.port = static_cast<uint16_t>(p);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            ep.host = authority.substr(0, colon);
            std::string port_str = authority.substr(colon + 1);
            long p = 0;
            try {
                p = std::stol(port_str);
            } catch (...) {
                *error = "端口号非法: " + port_str;
                return false;
            }
            if (p <= 0 || p > 65535) {
                *error = "端口号超出范围: " + port_str;
                return false;
            }
            ep.port = static_cast<uint16_t>(p);
        } else {
            ep.host = authority;
        }
    }

    if (ep.host.empty()) {
        *error = "endpoint 缺少主机名";
        return false;
    }
    if (ep.port == 0 && (ep.scheme == Scheme::Tcp || ep.scheme == Scheme::Udp ||
                         ep.scheme == Scheme::Rtp || ep.scheme == Scheme::Rtc)) {
        *error = std::string(scheme_name(ep.scheme)) + " 端点必须指定端口";
        return false;
    }
    if (ep.port == 0) {
        ep.port = scheme_default_port(ep.scheme);
    }

    *out = ep;
    return true;
}

}  // namespace kopnet
