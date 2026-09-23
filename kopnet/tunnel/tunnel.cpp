// KOPNET 隧道实现：单工作线程完成读分帧 / 控制协议 / credit 回压 / 写复用。
//
// 线程模型：应用线程只触碰 channel.send_queue（有界，背压入口）；
// 工作线程独占 rx 状态机、pending_writes、credit 计数与传输 I/O。
// 通道 map 用 mutex 保护增删，查找返回 shared_ptr 副本，避免在回压阻塞
// 期间持有锁。
#include "kopnet/tunnel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <climits>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "kop/log.h"
#include "kopnet/bounded_queue.hpp"
#include "tunnel_protocol.hpp"
#include "wire.hpp"

namespace kopnet {

namespace {

struct OutFrame {
    std::vector<uint8_t> bytes;
    std::vector<int> fds;
    size_t offset = 0;  // stream 传输已写字节（fd 只随首片发送）
};

struct Channel {
    uint32_t id = 0;
    ChannelMode mode = ChannelMode::Datagram;
    std::string kind;
    BoundedPacketQueue send_queue;
    BoundedPacketQueue recv_queue;
    DataHandler data_handler;
    // 以下仅工作线程访问
    int64_t send_credit = 0;     // 对端授予的剩余发送额度
    uint64_t recv_granted = 0;   // 已授予对端的接收额度
    uint64_t recv_delivered = 0;
    uint32_t next_send_seq = 0;
    bool opening = false;  // 等待 OPEN_ACK
    std::atomic<bool> closed{false};
    std::atomic<bool> open_ok{false};

    std::mutex open_mu;
    std::condition_variable open_cv;

    Channel(uint32_t id_, ChannelMode m, const std::string& k, size_t credit)
        : id(id_), mode(m), kind(k), send_queue(credit), recv_queue(credit) {}
};

}  // namespace

struct TunnelSession::Impl {
    Impl(std::unique_ptr<Transport> t, bool dialer, TunnelSession::Options o)
        : transport_(std::move(t)), is_dialer_(dialer), opts_(o) {}

    std::unique_ptr<Transport> transport_;
    const bool is_dialer_;
    const TunnelSession::Options opts_;

    std::atomic<bool> stop_{false};
    std::once_flag stop_once_;
    std::atomic<bool> alive_{false};
    std::atomic<bool> hello_ok_{false};
    std::atomic<bool> worker_done_{false};
    CloseHandler close_handler_;
    std::thread worker_;

    std::mutex channels_mu_;
    std::map<uint32_t, std::shared_ptr<Channel>> channels_;
    std::atomic<uint32_t> next_local_id_{0};

    // 待写帧（工作线程 + open_channel/close_channel 线程共享）
    std::mutex write_mu_;
    std::deque<OutFrame> pending_writes_;

    // 流式读状态机（仅工作线程）
    std::vector<uint8_t> rx_;
    size_t rx_target_ = KOPNET_TUNNEL_HEADER_SIZE;
    bool rx_have_header_ = false;
    TunnelHeader rx_hdr_;
    std::vector<int> rx_fds_;

    ChannelHandler channel_handler_;
    DataHandler default_data_handler_;
    TunnelLogger logger_;
    std::string last_error_;

    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> frames_received_{0};

    // 协商得到的对端限制
    uint32_t peer_max_frame_ = KOPNET_TUNNEL_MAX_FRAME;
    uint32_t peer_credit_ = KOPNET_TUNNEL_DEFAULT_CREDIT;
    bool peer_is_datagram_ = false;
    bool peer_closed_ = false;

    std::shared_ptr<Channel> find_channel(uint32_t id) {
        std::lock_guard<std::mutex> lock(channels_mu_);
        auto it = channels_.find(id);
        return it == channels_.end() ? nullptr : it->second;
    }

    void log(const std::string& msg) {
        if (logger_) {
            logger_(msg);
        } else {
            KOP_LOG_INFO("kopnet", "%s", msg.c_str());
        }
    }

    // ---- 控制帧提交（任意线程安全）----
    void submit_frame(OutFrame f) {
        {
            std::lock_guard<std::mutex> lock(write_mu_);
            pending_writes_.push_back(std::move(f));
        }
    }

    void send_control(ControlOp op, const void* msg) {
        std::vector<uint8_t> body = encode_control(op, msg);
        std::vector<uint8_t> payload = wrap_control(op, body);  // payload = op(4) + body
        OutFrame f;
        f.bytes = std::move(payload);
        TunnelHeader h;
        h.magic = KOPNET_TUNNEL_MAGIC;
        h.version = KOPNET_TUNNEL_VERSION;
        h.flags = KOPNET_TUNNEL_FLAG_CONTROL;
        h.channel_id = KOPNET_CONTROL_CHANNEL;
        h.payload_len = static_cast<uint32_t>(f.bytes.size());
        f.bytes.insert(f.bytes.begin(), KOPNET_TUNNEL_HEADER_SIZE, 0);
        encode_tunnel_header(h, f.bytes.data());
        submit_frame(std::move(f));
    }

    void send_credit_grant(const std::shared_ptr<Channel>& ch) {
        // CREDIT payload: op(4) + channel_id(4) + credit(4)
        WireWriter w;
        w.u32(ch->id);
        w.u32(opts_.recv_credit);
        OutFrame f;
        f.bytes = wrap_control(ControlOp::Credit, w.data());
        TunnelHeader h;
        h.magic = KOPNET_TUNNEL_MAGIC;
        h.version = KOPNET_TUNNEL_VERSION;
        h.flags = KOPNET_TUNNEL_FLAG_CONTROL;
        h.channel_id = KOPNET_CONTROL_CHANNEL;
        h.payload_len = static_cast<uint32_t>(f.bytes.size());
        f.bytes.insert(f.bytes.begin(), KOPNET_TUNNEL_HEADER_SIZE, 0);
        encode_tunnel_header(h, f.bytes.data());
        submit_frame(std::move(f));
        ch->recv_granted += opts_.recv_credit;
    }

    void maybe_grant(const std::shared_ptr<Channel>& ch) {
        uint64_t low = opts_.recv_credit / 2;
        if (ch->recv_granted <= ch->recv_delivered ||
            ch->recv_granted - ch->recv_delivered < low) {
            send_credit_grant(ch);
        }
    }

    uint32_t allocate_channel_id() {
        uint32_t base = is_dialer_ ? 1 : 2;
        while (true) {
            uint32_t id = base + 2 * (next_local_id_++);
            if (id == 0 || id == KOPNET_CONTROL_CHANNEL) continue;
            std::lock_guard<std::mutex> lock(channels_mu_);
            if (channels_.find(id) == channels_.end()) return id;
        }
    }

    bool mode_allowed(ChannelMode m) const {
        // Stream 通道要求可靠有序传输；datagram 传输只能开 Datagram 通道。
        if (m == ChannelMode::Stream && peer_is_datagram_) return false;
        return true;
    }

    // ---- 帧分发（工作线程）----
    void dispatch_frame(const TunnelHeader& h, const uint8_t* payload, size_t len,
                        const std::vector<int>& fds) {
        frames_received_.fetch_add(1, std::memory_order_relaxed);
        if (h.flags & KOPNET_TUNNEL_FLAG_CONTROL) {
            dispatch_control(payload, len);
            return;
        }
        auto ch = find_channel(h.channel_id);
        if (!ch || ch->closed.load()) {
            // 未知通道：协议错误，通知对端关闭
            ControlOpen close_msg;
            close_msg.channel_id = h.channel_id;
            send_control(ControlOp::Close, &close_msg);
            log("收到未知通道 " + std::to_string(h.channel_id) + " 的数据帧，已拒绝");
            return;
        }
        Packet pkt;
        pkt.data.assign(payload, payload + len);
        pkt.fds = fds;
        if (ch->data_handler) {
            ch->data_handler(ch->id, pkt.data, pkt.fds);
        } else {
            // 有界入队，带停止检查的阻塞（回压）
            while (!stop_.load()) {
                QueueStatus qs = ch->recv_queue.put(std::move(pkt), 50);
                if (qs == QueueStatus::Ok) break;
                if (qs == QueueStatus::Closed) break;
                // Timeout：继续等待（应用未消费）
            }
        }
        ch->recv_delivered++;
        maybe_grant(ch);
    }

    void dispatch_control(const uint8_t* payload, size_t len) {
        std::vector<uint8_t> pv(payload, payload + len);
        ControlOp op = ControlOp::Hello;
        ControlHello hello;
        ControlHelloAck ack;
        ControlOpen open;
        ControlOpenAck open_ack;
        uint32_t channel_id = 0;
        uint32_t credit_value = 0;
        std::string error;
        if (!decode_control(pv, &op, &hello, &ack, &open, &open_ack, &channel_id,
                            &credit_value, &error)) {
            log("控制帧解码失败: " + error);
            return;
        }
        switch (op) {
            case ControlOp::Hello: {
                peer_max_frame_ = std::min<uint32_t>(peer_max_frame_, hello.max_frame);
                peer_credit_ = hello.credit;
                ControlHelloAck our_ack;
                our_ack.status = 0;
                our_ack.sel_major = KOPNET_TUNNEL_VERSION;
                our_ack.sel_minor = 0;
                our_ack.capabilities =
                    KOPNET_TUNNEL_CAP_DATAGRAM_CHANNELS | KOPNET_TUNNEL_CAP_STREAM_CHANNELS;
                if (transport_->supports_fds()) {
                    our_ack.capabilities |= KOPNET_TUNNEL_CAP_FD_PASSING;
                }
                our_ack.max_channels = opts_.max_channels;
                our_ack.credit = opts_.recv_credit;
                our_ack.max_frame = opts_.max_frame;
                send_control(ControlOp::HelloAck, &our_ack);
                break;
            }
            case ControlOp::HelloAck: {
                if (ack.status == 0) {
                    peer_max_frame_ = std::min<uint32_t>(peer_max_frame_, ack.max_frame);
                    peer_credit_ = ack.credit;
                    hello_ok_.store(true);
                } else {
                    last_error_ = "对端拒绝 HELLO";
                    log(last_error_);
                    stop_.store(true);
                }
                break;
            }
            case ControlOp::Open: {
                if (channels_.size() >= opts_.max_channels) {
                    ControlOpenAck refuse;
                    refuse.channel_id = open.channel_id;
                    refuse.status = 1;
                    refuse.mode = open.mode;
                    send_control(ControlOp::OpenAck, &refuse);
                    break;
                }
                if (!mode_allowed(open.mode) || open.channel_id == 0 ||
                    open.channel_id == KOPNET_CONTROL_CHANNEL) {
                    ControlOpenAck refuse;
                    refuse.channel_id = open.channel_id;
                    refuse.status = 1;
                    refuse.mode = open.mode;
                    send_control(ControlOp::OpenAck, &refuse);
                    log("拒绝通道 " + std::to_string(open.channel_id) + "：模式/id 非法");
                    break;
                }
                auto existing = find_channel(open.channel_id);
                if (existing) {
                    ControlOpenAck refuse;
                    refuse.channel_id = open.channel_id;
                    refuse.status = 1;
                    refuse.mode = open.mode;
                    send_control(ControlOp::OpenAck, &refuse);
                    break;
                }
                auto ch = std::make_shared<Channel>(open.channel_id, open.mode, open.kind,
                                                    opts_.recv_credit);
                ch->open_ok.store(true, std::memory_order_release);  // 接收侧通道立即可用
                {
                    std::lock_guard<std::mutex> lock(channels_mu_);
                    channels_[open.channel_id] = ch;
                }
                // 先通知应用装好 data handler，再回 OPEN_ACK 并授予额度：
                // 对端只有收到 ACK+credit 才可能发数据，否则首帧可能抢在
                // 应用注册回调之前落入 recv_queue，造成回调流与队列流乱序。
                if (channel_handler_) channel_handler_(ch->id, ch->kind, ch->mode);
                ControlOpenAck ok_ack;
                ok_ack.channel_id = open.channel_id;
                ok_ack.status = 0;
                ok_ack.mode = open.mode;
                send_control(ControlOp::OpenAck, &ok_ack);
                // 接收侧授予初始额度
                send_credit_grant(ch);
                break;
            }
            case ControlOp::OpenAck: {
                auto ch = find_channel(open_ack.channel_id);
                if (!ch) break;
                {
                    std::lock_guard<std::mutex> lock(ch->open_mu);
                    if (open_ack.status == 0) {
                        ch->open_ok.store(true, std::memory_order_release);
                    } else {
                        ch->closed.store(true, std::memory_order_release);
                    }
                }
                // 打开侧也向对端授予初始接收额度，双向都能发包
                if (open_ack.status == 0) send_credit_grant(ch);
                ch->open_cv.notify_all();
                break;
            }
            case ControlOp::Close: {
                auto ch = find_channel(channel_id);
                if (ch) {
                    ch->closed.store(true);
                    ch->recv_queue.close();
                    {
                        std::lock_guard<std::mutex> lock(ch->open_mu);
                        ch->open_ok.store(false, std::memory_order_release);
                    }
                    ch->open_cv.notify_all();
                    std::lock_guard<std::mutex> lock(channels_mu_);
                    channels_.erase(channel_id);
                }
                break;
            }
            case ControlOp::Credit: {
                auto ch = find_channel(channel_id);
                if (ch) {
                    ch->send_credit += static_cast<int64_t>(credit_value);
                }
                break;
            }
            case ControlOp::Ping: {
                send_control(ControlOp::Pong, &open);
                break;
            }
            case ControlOp::Pong:
                break;
            case ControlOp::Goodbye:
                log("对端发送 GOODBYE");
                stop_.store(true);
                break;
        }
    }

    // ---- 读（工作线程）----
    enum class ReadResult { Ok, WouldBlock, Closed, Error };

    ReadResult pump_read_once() {
        if (transport_->semantics() == TransportSemantics::Datagram) {
            std::vector<uint8_t> data;
            std::vector<int> fds;
            std::string error;
            IoStatus st = transport_->recv_datagram(&data, &fds, &error);
            if (st == IoStatus::WouldBlock) return ReadResult::WouldBlock;
            if (st == IoStatus::Closed) return ReadResult::Closed;
            if (st != IoStatus::Ok) {
                last_error_ = error;
                log("recv_datagram 错误: " + error);
                return ReadResult::Error;
            }
            TunnelHeader h;
            if (!decode_tunnel_header(data.data(), data.size(), &h) ||
                h.magic != KOPNET_TUNNEL_MAGIC ||
                data.size() < KOPNET_TUNNEL_HEADER_SIZE + h.payload_len) {
                log("数据报帧非法，丢弃");
                return ReadResult::Ok;
            }
            dispatch_frame(h, data.data() + KOPNET_TUNNEL_HEADER_SIZE, h.payload_len, fds);
            return ReadResult::Ok;
        }
        // 流式精确读取：每次 recvmsg 最多读到当前帧边界，保证 fd 归属正确
        size_t want = rx_target_ - rx_.size();
        if (want == 0) {
            rx_.clear();
            rx_have_header_ = false;
            rx_target_ = KOPNET_TUNNEL_HEADER_SIZE;
            want = KOPNET_TUNNEL_HEADER_SIZE;
        }
        uint8_t buf[KOPNET_TUNNEL_MAX_FRAME];
        std::vector<int> fds;
        std::string error;
        size_t n = 0;
        IoStatus st = transport_->read(buf, want, &n, &fds, &error);
        if (st == IoStatus::WouldBlock) return ReadResult::WouldBlock;
        if (st == IoStatus::Closed) return ReadResult::Closed;
        if (st != IoStatus::Ok) {
            last_error_ = error;
            log("read 错误: " + error);
            return ReadResult::Error;
        }
        if (!fds.empty()) rx_fds_.insert(rx_fds_.end(), fds.begin(), fds.end());
        rx_.insert(rx_.end(), buf, buf + n);
        if (rx_.size() < rx_target_) return ReadResult::Ok;
        if (!rx_have_header_) {
            if (!decode_tunnel_header(rx_.data(), rx_.size(), &rx_hdr_) ||
                rx_hdr_.magic != KOPNET_TUNNEL_MAGIC ||
                rx_hdr_.version != KOPNET_TUNNEL_VERSION ||
                rx_hdr_.payload_len > opts_.max_frame) {
                last_error_ = "隧道帧头非法";
                log(last_error_);
                return ReadResult::Error;
            }
            rx_have_header_ = true;
            rx_target_ = KOPNET_TUNNEL_HEADER_SIZE + rx_hdr_.payload_len;
            if (rx_.size() < rx_target_) return ReadResult::Ok;
        }
        // 整帧完成
        dispatch_frame(rx_hdr_, rx_.data() + KOPNET_TUNNEL_HEADER_SIZE,
                       rx_hdr_.payload_len, rx_fds_);
        rx_.clear();
        rx_fds_.clear();
        rx_have_header_ = false;
        rx_target_ = KOPNET_TUNNEL_HEADER_SIZE;
        return ReadResult::Ok;
    }

    // ---- 通道发送队列 → 待写帧（工作线程）----
    void pump_channel_sends() {
        std::vector<std::shared_ptr<Channel>> snapshot;
        {
            std::lock_guard<std::mutex> lock(channels_mu_);
            for (const auto& kv : channels_) snapshot.push_back(kv.second);
        }
        for (auto& ch : snapshot) {
            if (ch->closed.load() || !ch->open_ok.load(std::memory_order_acquire)) continue;
            while (ch->send_credit > 0) {
                Packet pkt;
                QueueStatus qs = ch->send_queue.try_get(&pkt);
                if (qs != QueueStatus::Ok) break;
                OutFrame f;
                TunnelHeader h;
                h.magic = KOPNET_TUNNEL_MAGIC;
                h.version = KOPNET_TUNNEL_VERSION;
                h.flags = KOPNET_TUNNEL_FLAG_NONE;
                h.channel_id = static_cast<uint8_t>(ch->id);
                h.sequence = ch->next_send_seq++;
                h.payload_len = static_cast<uint32_t>(pkt.data.size());
                h.fd_count = static_cast<uint32_t>(pkt.fds.size());
                f.bytes.resize(KOPNET_TUNNEL_HEADER_SIZE);
                encode_tunnel_header(h, f.bytes.data());
                f.bytes.insert(f.bytes.end(), pkt.data.begin(), pkt.data.end());
                f.fds = std::move(pkt.fds);
                submit_frame(std::move(f));
                ch->send_credit--;
            }
        }
    }

    // 已发出/已丢弃的帧：关闭其携带的 fd（成功时 fd 已随 SCM_RIGHTS 移交对端，
    // 这里关的是本进程的引用副本）。
    static void close_frame_fds(OutFrame* f) {
        for (int fd : f->fds) {
            if (fd >= 0) ::close(fd);
        }
        f->fds.clear();
    }

    // ---- 写（工作线程）----
    bool flush_writes() {
        while (true) {
            OutFrame f;
            {
                std::lock_guard<std::mutex> lock(write_mu_);
                if (pending_writes_.empty()) return true;
                f = std::move(pending_writes_.front());
                pending_writes_.pop_front();
            }
            std::string error;
            if (transport_->semantics() == TransportSemantics::Datagram) {
                const std::vector<int>* fdsptr = f.fds.empty() ? nullptr : &f.fds;
                IoStatus st = transport_->send_datagram(f.bytes.data(), f.bytes.size(), fdsptr,
                                                        &error);
                if (st == IoStatus::WouldBlock) {
                    // 放回队首，等工作循环等可写后重试（WouldBlock 不是错误）
                    std::lock_guard<std::mutex> lock(write_mu_);
                    pending_writes_.push_front(std::move(f));
                    return true;
                }
                if (st != IoStatus::Ok) {
                    last_error_ = error;
                    log("send_datagram 错误: " + error);
                    close_frame_fds(&f);
                    return false;
                }
                frames_sent_.fetch_add(1, std::memory_order_relaxed);
                close_frame_fds(&f);
                continue;
            }
            // 流式：允许部分写，fd 只随首片
            while (f.offset < f.bytes.size()) {
                size_t n = 0;
                const std::vector<int>* fdsptr =
                    (f.offset == 0 && !f.fds.empty()) ? &f.fds : nullptr;
                IoStatus st = transport_->write(f.bytes.data() + f.offset,
                                                f.bytes.size() - f.offset, &n, fdsptr, &error);
                if (st == IoStatus::WouldBlock) {
                    // 留在队首，等工作循环等可写后重试（WouldBlock 不是错误）
                    std::lock_guard<std::mutex> lock(write_mu_);
                    pending_writes_.push_front(std::move(f));
                    return true;
                }
                if (st != IoStatus::Ok) {
                    last_error_ = error;
                    log("write 错误: " + error);
                    close_frame_fds(&f);
                    return false;
                }
                f.offset += n;
            }
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
            close_frame_fds(&f);
        }
    }

    bool has_pending_writes() {
        std::lock_guard<std::mutex> lock(write_mu_);
        return !pending_writes_.empty();
    }

    void close_all_channels() {
        std::lock_guard<std::mutex> lock(channels_mu_);
        for (auto& kv : channels_) {
            kv.second->closed.store(true);
            kv.second->recv_queue.close();
            {
                std::lock_guard<std::mutex> l(kv.second->open_mu);
                kv.second->open_ok.store(false, std::memory_order_release);
            }
            kv.second->open_cv.notify_all();
        }
        channels_.clear();
    }

    void worker_loop() {
        peer_is_datagram_ =
            transport_->semantics() == TransportSemantics::Datagram;
        // HELLO
        ControlHello hello;
        hello.ver_major = KOPNET_TUNNEL_VERSION;
        hello.ver_minor = 0;
        hello.capabilities = KOPNET_TUNNEL_CAP_DATAGRAM_CHANNELS |
                             KOPNET_TUNNEL_CAP_STREAM_CHANNELS;
        if (transport_->supports_fds()) hello.capabilities |= KOPNET_TUNNEL_CAP_FD_PASSING;
        hello.max_channels = opts_.max_channels;
        hello.credit = opts_.recv_credit;
        hello.max_frame = opts_.max_frame;
        send_control(ControlOp::Hello, &hello);
        alive_.store(true);
        const auto hello_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(opts_.handshake_ms);

        while (!stop_.load()) {
            if (!hello_ok_.load(std::memory_order_acquire) &&
                std::chrono::steady_clock::now() >= hello_deadline) {
                last_error_ = "HELLO 协商超时";
                log(last_error_);
                stop_.store(true);
                break;
            }
            // 读到无数据为止
            while (!stop_.load()) {
                ReadResult rr = pump_read_once();
                if (rr == ReadResult::WouldBlock) break;
                if (rr == ReadResult::Closed) {
                    log("传输对端关闭");
                    peer_closed_ = true;
                    stop_.store(true);
                    break;
                }
                if (rr == ReadResult::Error) {
                    stop_.store(true);
                    break;
                }
            }
            if (stop_.load()) break;
            pump_channel_sends();
            bool write_ok = flush_writes();
            if (!write_ok) {
                stop_.store(true);
                break;
            }
            // 有序等待：有待写则等可写，否则等可读（带超时以响应停止）
            std::string error;
            if (has_pending_writes()) {
                transport_->wait_writable(20, &error);
            } else {
                transport_->wait_readable(20, &error);
            }
        }
        // 优雅停机收尾：把已入队但尚未写出的帧冲刷出去（GOODBYE 等停机
        // 消息依赖这一点真正到达对端）。对端已断开时跳过——写了也只能得到
        // EPIPE，滞留帧的 fd 由下方既有逻辑回收。
        if (!peer_closed_) {
            pump_channel_sends();
            const auto flush_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(200);
            while (std::chrono::steady_clock::now() < flush_deadline) {
                if (!flush_writes()) break;
                if (!has_pending_writes()) break;
                std::string wait_error;
                transport_->wait_writable(10, &wait_error);
            }
        }
        close_all_channels();
        if (close_handler_) close_handler_();
        // 滞留未发的帧：fd 由本端回收
        {
            std::lock_guard<std::mutex> lock(write_mu_);
            for (auto& f : pending_writes_) close_frame_fds(&f);
            pending_writes_.clear();
        }
        alive_.store(false);
        worker_done_.store(true);
    }
};

TunnelSession::TunnelSession() = default;

std::unique_ptr<TunnelSession> TunnelSession::create(std::unique_ptr<Transport> transport,
                                                     bool is_dialer, Options opts,
                                                     std::string* error) {
    if (!transport || !transport->valid()) {
        if (error) *error = "传输无效";
        return nullptr;
    }
    auto session = std::unique_ptr<TunnelSession>(new TunnelSession);
    session->impl_ = std::make_unique<Impl>(std::move(transport), is_dialer, opts);
    return session;
}

TunnelSession::~TunnelSession() {
    stop();
    if (impl_) {
        if (impl_->transport_) impl_->transport_->close();
    }
}

bool TunnelSession::start(std::string* error) {
    if (!impl_) return false;
    if (impl_->worker_.joinable()) return true;
    impl_->stop_.store(false);
    impl_->worker_done_.store(false);
    impl_->worker_ = std::thread([this] { impl_->worker_loop(); });
    (void)error;
    return true;
}

// 等待 HELLO 协商完成（open_channel/recv 前置条件）
bool TunnelSession::wait_hello(int timeout_ms, std::string* error) {
    if (!impl_) return false;
    if (impl_->hello_ok_.load(std::memory_order_acquire)) return true;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    while (!impl_->hello_ok_.load(std::memory_order_acquire)) {
        if (impl_->worker_done_.load(std::memory_order_acquire)) {
            if (error)
                *error = impl_->last_error_.empty() ? "隧道工作线程退出" : impl_->last_error_;
            return false;
        }
        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            if (error) *error = "HELLO 协商超时";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void TunnelSession::stop() {
    if (!impl_) return;
    // 幂等 + 线程安全：应用层可能在 disconnect() 与析构两条路径上各调一次，
    // 甚至并发调用（call_once 保证只真正停机一次，join 不会重入）
    std::call_once(impl_->stop_once_, [this] {
        impl_->stop_.store(true);
        if (impl_->worker_.joinable()) {
            impl_->worker_.join();
        }
        impl_->close_all_channels();
    });
}

bool TunnelSession::alive() const {
    return impl_ && impl_->alive_.load() && !impl_->stop_.load();
}

void TunnelSession::set_channel_handler(ChannelHandler h) {
    impl_->channel_handler_ = std::move(h);
}

void TunnelSession::set_data_handler(uint32_t id, DataHandler h) {
    auto ch = impl_->find_channel(id);
    if (ch) ch->data_handler = std::move(h);
}

void TunnelSession::set_close_handler(CloseHandler h) {
    impl_->close_handler_ = std::move(h);
}

void TunnelSession::set_logger(TunnelLogger h) { impl_->logger_ = std::move(h); }

uint32_t TunnelSession::open_channel(const std::string& kind, ChannelMode mode,
                                     int timeout_ms, std::string* error) {
    return open_channel(kind, mode, timeout_ms, {}, error);
}

uint32_t TunnelSession::open_channel(const std::string& kind, ChannelMode mode,
                                     int timeout_ms, DataHandler handler,
                                     std::string* error) {
    if (!impl_) return 0;
    // HELLO 未完成时先等（工作线程已并发开始协商）
    if (!impl_->hello_ok_.load(std::memory_order_acquire) &&
        !wait_hello(timeout_ms > 0 ? timeout_ms : impl_->opts_.handshake_ms, error)) {
        return 0;
    }
    if (!impl_->mode_allowed(mode)) {
        if (error)
            *error = std::string("通道模式 [") + channel_mode_name(mode) +
                     "] 与当前传输语义不兼容";
        return 0;
    }
    uint32_t id = impl_->allocate_channel_id();
    auto ch = std::make_shared<Channel>(id, mode, kind, impl_->opts_.recv_credit);
    // 先装 data handler 再登记通道、发 OPEN：对端只有收到 OPEN_ACK 后才可能
    // 回数据，因此首帧到达时回调必然已就位，不会先落入 recv_queue。
    ch->data_handler = std::move(handler);
    ch->opening = true;
    {
        std::lock_guard<std::mutex> lock(impl_->channels_mu_);
        impl_->channels_[id] = ch;
    }
    ControlOpen open;
    open.channel_id = id;
    open.mode = mode;
    open.kind = kind;
    impl_->send_control(ControlOp::Open, &open);

    std::unique_lock<std::mutex> lock(ch->open_mu);
    if (ch->open_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&] {
                                 return ch->open_ok.load(std::memory_order_acquire) ||
                                        ch->closed.load(std::memory_order_acquire);
                             })) {
        if (ch->open_ok.load()) return id;
        if (error) *error = "对端拒绝打开通道";
        return 0;
    }
    if (error) *error = "打开通道超时";
    {
        std::lock_guard<std::mutex> l2(impl_->channels_mu_);
        impl_->channels_.erase(id);
    }
    return 0;
}


void TunnelSession::close_channel(uint32_t id) {
    if (!impl_) return;
    auto ch = impl_->find_channel(id);
    if (!ch) return;
    ControlOpen close_msg;
    close_msg.channel_id = id;
    impl_->send_control(ControlOp::Close, &close_msg);
    ch->closed.store(true);
    ch->recv_queue.close();
    {
        std::lock_guard<std::mutex> lock(ch->open_mu);
        ch->open_ok.store(false, std::memory_order_release);
    }
    ch->open_cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(impl_->channels_mu_);
        impl_->channels_.erase(id);
    }
}

SendStatus TunnelSession::send(uint32_t id, const uint8_t* data, size_t len,
                               const std::vector<int>* fds, int timeout_ms) {
    if (!impl_ || !impl_->alive_.load()) return SendStatus::Closed;
    if (len > KOPNET_TUNNEL_MAX_PAYLOAD) return SendStatus::Error;
    auto ch = impl_->find_channel(id);
    if (!ch || ch->closed.load()) return SendStatus::Closed;
    if (!ch->open_ok.load(std::memory_order_acquire)) {
        // 通道尚未确认；等待其建立或超时
        std::unique_lock<std::mutex> lock(ch->open_mu);
        ch->open_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 2000),
                             [&] {
                                 return ch->open_ok.load(std::memory_order_acquire) ||
                                        ch->closed.load(std::memory_order_acquire);
                             });
        if (!ch->open_ok.load()) return SendStatus::Timeout;
    }
    Packet pkt;
    pkt.data.assign(data, data + len);
    if (fds) pkt.fds = *fds;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms < 0 ? INT32_MAX : timeout_ms);
    while (!impl_->stop_.load()) {
        QueueStatus qs = ch->send_queue.put(std::move(pkt), 50);
        if (qs == QueueStatus::Ok) return SendStatus::Ok;
        if (qs == QueueStatus::Closed) return SendStatus::Closed;
        // Timeout：背压等待，直到总超时
        if (std::chrono::steady_clock::now() >= deadline) return SendStatus::Timeout;
    }
    return SendStatus::Closed;
}

RecvStatus TunnelSession::recv(uint32_t id, std::vector<uint8_t>* data,
                               std::vector<int>* fds, int timeout_ms) {
    if (!impl_) return RecvStatus::Closed;
    auto ch = impl_->find_channel(id);
    if (!ch || ch->closed.load()) return RecvStatus::Closed;
    Packet pkt;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms < 0 ? INT32_MAX : timeout_ms);
    while (!impl_->stop_.load()) {
        QueueStatus qs = ch->recv_queue.get(&pkt, 50);
        if (qs == QueueStatus::Ok) {            *data = std::move(pkt.data);
            if (fds) *fds = std::move(pkt.fds);
            return RecvStatus::Ok;
        }
        if (qs == QueueStatus::Closed) return RecvStatus::Closed;
        if (std::chrono::steady_clock::now() >= deadline) return RecvStatus::Timeout;
    }
    return RecvStatus::Closed;
}

uint64_t TunnelSession::frames_sent() const {
    return impl_ ? impl_->frames_sent_.load() : 0;
}

uint64_t TunnelSession::frames_received() const {
    return impl_ ? impl_->frames_received_.load() : 0;
}

size_t TunnelSession::channel_count() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->channels_mu_);
    return impl_->channels_.size();
}

Transport* TunnelSession::transport() const {
    return impl_ ? impl_->transport_.get() : nullptr;
}

}  // namespace kopnet
