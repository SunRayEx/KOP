// KOPNET 协议层：RTP 适配器（rtp://host:port?pt=96&ssrc=..&clock=90000）。
//
// 把“一条 RTP 媒体流”包装成 Datagram 语义的 Transport，供 Tunnel 骑乘：
//   发送：应用给出的每个数据报 = 一个 RTP 包的负载，本模块加 RTP 头
//        （V2、可配 PT/SSRC、序号自增、时间戳按配置步进）后经 UDP 发出。
//        因此 KOPNET 对端可以是任意标准 RTP 接收者。
//   接收：解析 RTP 头、剥离 CSRC/扩展头/padding 后把负载交给应用；
//        按 16bit 序号统计丢包与重复（不做重排，按到达顺序投递）。
//        RTCP/STUN/其它报文若复用到同一端口会被识别并跳过（计入 stats）。
//
// 限制（纯用户态、无外部媒体栈）：不做分片/重组——负载 + 12 字节头不能
// 超过 KOPNET_MAX_DATAGRAM；典型以太网 MTU 下建议负载不超过 ~1400 字节，
// 超出时分片应由上层的负载格式（如 KOPAW 帧信封）负责。不生成 RTCP
// （SR/RR 反馈不由 KOPNET 产生）；收到 RTCP 只统计不投递。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kopnet/transport.hpp"
#include "kopnet/transport_listener.hpp"
#include "rtp_framing.hpp"

namespace kopnet {

struct RtpConfig {
    // 负载类型。动态范围 96..127；默认 96（KOPMS 远程桌面占位）。
    uint8_t payload_type = 96;
    // 同步源。0 = 启动时随机生成（RFC 3550 建议）。
    uint32_t ssrc = 0;
    // 媒体时钟频率（Hz），仅用于换算默认时间戳步进。
    uint32_t clock_rate = 90000;
    // 每个数据报的时间戳增量。0 = 按 clock_rate/fps 计算（默认 fps=30）。
    uint32_t ts_step = 0;
    // 时间戳基准 fps（ts_step 为 0 时使用）。
    uint32_t fps = 30;
    // 是否在每个 RTP 包置 marker 位（帧末标记；不支持分片时整包即一帧，
    // 默认置位）。
    bool marker = true;
};

struct RtpStats {
    uint64_t sent = 0;         // 成功发出的 RTP 包数
    uint64_t received = 0;     // 投递给应用的 RTP 负载数
    uint64_t lost = 0;         // 序列号前向间隙统计的丢失包数
    uint64_t duplicates = 0;   // 重复/乱序回退的包数
    uint64_t rtcp_skipped = 0; // 收到并跳过的 RTCP 包数
    uint64_t other_skipped = 0;// 无法识别的报文数
};

class RtpTransport final : public Transport {
public:
    RtpTransport() = default;
    ~RtpTransport() override { close(); }

    RtpTransport(const RtpTransport&) = delete;
    RtpTransport& operator=(const RtpTransport&) = delete;
    RtpTransport(RtpTransport&&) = delete;
    RtpTransport& operator=(RtpTransport&&) = delete;

    // 接管一个已连接好的 UDP socket fd（由 udp_connect / 会话化 listen 产出）。
    // pending 为 listen 侧已收下的首个报文（避免丢失），其内容会被当作
    // 第一个 RTP 包在首次 recv_datagram 时处理。
    void attach(int fd, std::vector<uint8_t> pending, RtpConfig config,
                std::string desc);

    TransportSemantics semantics() const override { return TransportSemantics::Datagram; }
    bool supports_fds() const override { return false; }
    bool valid() const override { return fd_ >= 0; }
    void close() override;
    std::string describe() const override { return desc_; }

    // 数据报传输不提供流式语义
    IoStatus read(uint8_t* /*buf*/, size_t /*cap*/, size_t* /*n*/, std::vector<int>* /*fds*/,
                  std::string* error) override;
    IoStatus write(const uint8_t* /*buf*/, size_t /*len*/, size_t* /*n*/,
                   const std::vector<int>* /*fds*/, std::string* error) override;

    IoStatus send_datagram(const uint8_t* data, size_t len, const std::vector<int>* fds,
                           std::string* error) override;
    IoStatus recv_datagram(std::vector<uint8_t>* data, std::vector<int>* fds,
                           std::string* error) override;

    IoStatus wait_readable(int timeout_ms, std::string* error) override;
    IoStatus wait_writable(int timeout_ms, std::string* error) override;

    RtpConfig config() const { return config_; }
    RtpStats stats() const { return stats_; }
    // 本端发送所用 SSRC（配置为 0 时是随机生成的值）。
    uint32_t local_ssrc() const { return config_.ssrc; }
    // 对端 RTP SSRC（首个有效 RTP 包到达后才有意义；否则 0）。
    uint32_t peer_ssrc() const { return peer_ssrc_; }
    // 下一个待发序号（诊断用）。
    uint16_t next_send_sequence() const { return send_seq_; }

private:
    IoStatus handle_packet(const uint8_t* pkt, size_t len, std::vector<uint8_t>* data,
                           std::string* error);

    int fd_ = -1;
    std::string desc_;
    RtpConfig config_{};
    RtpStats stats_{};
    std::vector<uint8_t> pending_;  // listen 侧首包

    uint16_t send_seq_ = 0;     // 下一个待发序号
    uint32_t send_ts_ = 0;      // 下一个待发时间戳
    uint32_t send_ts_step_ = 0; // 每包时间戳增量

    bool recv_synced_ = false;     // 是否已锁定对端序号基准
    uint16_t recv_expected_ = 0;   // 下一个期望序号
    uint32_t peer_ssrc_ = 0;
};

// Connect 角色：建立到 host:port 的 UDP 5 元组并包装成 RTP 传输。
std::unique_ptr<Transport> rtp_connect(const std::string& host, uint16_t port,
                                       const RtpConfig& config, std::string* error);

// Serve 角色：在 host:port 上监听，首个报文到达时把 socket 会话化
// （connect 到该对端）并包装成 RTP 传输。
std::unique_ptr<TransportListener> rtp_listen(const std::string& host, uint16_t port,
                                              const RtpConfig& config, std::string* error);

}  // namespace kopnet
