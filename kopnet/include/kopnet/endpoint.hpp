// KOPNET 透明网络架构：端点（Endpoint）寻址。
//
// 远程控制协议（SSH/RTC/RTP/UDP/RDP 等）统一被表达成一个 URI 端点。
// 上层（KOPAW 节点 / KOPMS 远程会话）只认 Endpoint，不接触 socket、协议
// 握手或分帧细节——这是“透明”的第一层边界。scheme 选择 ProtocolAdapter，
// adapter 把“该协议的一条连接”变成一个 Transport 供 Tunnel 骑乘。
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace kopnet {

// 传输语义分类。adapter 产出 Transport 时声明其语义，Tunnel 据此选择
// 分帧策略（流式 = 显式长度前缀；数据报 = 每报文一帧）。
enum class TransportSemantics {
    Stream,      // 可靠有序字节流（TCP / AF_UNIX stream / SSH 管道 / RDP 连接）
    Datagram,    // 保留边界的不可靠数据报（UDP / RTP / RTC）
};

// 远程控制协议。新增协议时在此追加 scheme 并实现对应 adapter。
enum class Scheme {
    Unknown,
    Tcp,     // tcp://host:port —— 明文 TCP，基准传输
    Udp,     // udp://host:port —— 无连接数据报
    Unix,    // unix:name 或 unix:///abs/path —— 本地 AF_UNIX
    Ssh,     // ssh://[user@]host[:port]/远程命令 —— 经 ssh 子进程建立安全通道
    Rtp,     // rtp://host:port?pt=..&ssrc=..&clock=.. —— RTP 媒体数据报
    Rtc,     // rtc://host:port?sid=.. —— 单 5 元组会话（STUN/RTP/RTCP 复用）
    Rdp,     // rdp://host:port —— TPKT/X.224 连接内承载 KOPNET 隧道
    Relay,   // relay://name —— 连接到本地/远端 kopnet-relay 中继
};

// 角色：Connect = 主动拨向端点；Serve = 在端点上接受连接。
enum class Role {
    Connect,
    Serve,
};

// 一个已解析的端点。字段按 scheme 有选择地有效。
struct Endpoint {
    Scheme scheme = Scheme::Unknown;
    Role role = Role::Connect;
    std::string user;   // ssh 的 user@ 部分
    std::string host;   // 主机名/地址；Unix 为 socket 名或绝对路径
    uint16_t port = 0;  // 端口；Unix/Relay 无意义
    std::string path;   // ssh 远程命令（URL 解码后）；其余 scheme 为空
    std::string raw;    // 原始 URI（诊断）
    // 查询参数（key 已小写）。RTP/RTC 的 pt/ssrc/clock/sid 等从这里读。
    std::map<std::string, std::string> params;

    // 便捷取参（key 大小写不敏感）。
    std::string param(const std::string& key) const;
    long param_int(const std::string& key, long fallback) const;
};

// 解析远程控制 URI。失败时返回 false 并填充 error。
//
// 支持形式：
//   tcp://host[:port]
//   udp://host[:port]
//   unix:name              （XDG_RUNTIME_DIR 相对，沿用 BUS2LAYER 约定）
//   unix:///abs/path       （绝对路径）
//   ssh://[user@]host[:port]/remote-command[?arg=..]
//   rtp://host:port[?pt=96&ssrc=..&clock=90000&fps=30&ts_step=..]
//   rtc://host:port[?sid=..]
//   rdp://host[:port]
//   relay://name
//
// 所有 scheme 都可追加 ?key=value&key2=value2 查询参数。
bool endpoint_parse(const std::string& uri, Endpoint* out, std::string* error);

// scheme 名称（用于日志与 CLI）。
const char* scheme_name(Scheme s);
// 该 scheme 的传输语义（adapter 产出的 Transport 类型）。
TransportSemantics scheme_semantics(Scheme s);
// 该 scheme 是否支持 Serve 角色（监听）。
bool scheme_supports_serve(Scheme s);

// 默认端口（无端口指定时使用；rdp=3389，其余 scheme 各自默认）。
uint16_t scheme_default_port(Scheme s);

}  // namespace kopnet
