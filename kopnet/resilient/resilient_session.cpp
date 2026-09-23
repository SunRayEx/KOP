// KOPNET 会话韧性层实现。
#include "kopnet/resilient.hpp"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <thread>

#include "kop/log.h"
#include "kopnet/adapters.hpp"

namespace kopnet {

namespace {
const char* kTag = "kopnet-resilient";
}

struct ResilientSession::Impl {
    Options opts;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};

    mutable std::mutex mu;
    std::condition_variable cv;

    struct ChannelReg {
        std::string kind;
        ChannelMode mode = ChannelMode::Stream;
        uint32_t wire_id = 0;  // 当前隧道上的通道号；0 = 尚未打开
        bool closed = false;   // close_channel 后不再随重连重开
        DataHandler handler;   // push 处理器：随通道重开自动重装
    };
    std::vector<ChannelReg> channels_;
    std::shared_ptr<TunnelSession> current_;
    // 当前会话的“已关闭”标记（close handler 置位）：只作唤醒信号，
    // 不直接改 connected_——否则 set_disconnected 无法判断是否已宣布过连接
    std::shared_ptr<std::atomic<bool>> session_closed_;
    bool connected_ = false;
    uint64_t epoch_ = 0;  // 每条新连接自增；用于丢弃已淘汰会话的通道号

    ChannelHandler channel_handler_;
    StateHandler state_handler_;
    CloseHandler close_handler_;
    TunnelLogger logger_;
    std::atomic<bool> close_fired_{false};
    std::thread worker_;
    std::atomic<bool> worker_done_{false};

    // 每条会话自带一个“已关闭”标记：close handler 与 set_connected 通过它
    // 通信“握手刚完成传输就断了”这一竞态
    struct DialResult {
        std::shared_ptr<TunnelSession> session;
        std::shared_ptr<std::atomic<bool>> closed;
    };

    void loop();
    bool dial_once(const std::string& uri, DialResult* out, std::string* error);
    bool set_connected(DialResult dr, uint64_t epoch);
    void set_disconnected();
    void notify_dead();
    void fire_close();
    void reopen(const std::shared_ptr<TunnelSession>& session);
    bool sleep_interruptible(uint32_t ms);
    uint32_t backoff(uint32_t attempt) const;
    bool channel_lookup(uint32_t logical, uint32_t* wire) const;
};

uint32_t ResilientSession::Impl::backoff(uint32_t attempt) const {
    if (attempt == 0) return 0;
    const uint32_t shift = std::min(attempt - 1, 6u);
    const uint64_t delay = static_cast<uint64_t>(opts.base_delay_ms) << shift;
    return static_cast<uint32_t>(std::min(delay, static_cast<uint64_t>(opts.max_delay_ms)));
}

bool ResilientSession::Impl::sleep_interruptible(uint32_t ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return true;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (left > std::chrono::milliseconds(50)) left = std::chrono::milliseconds(50);
        std::this_thread::sleep_for(left);
    }
    return false;
}

bool ResilientSession::Impl::channel_lookup(uint32_t logical, uint32_t* wire) const {
    // 调用方持 mu
    if (logical == 0 || logical > channels_.size()) return false;
    *wire = channels_[logical - 1].wire_id;
    return true;
}

bool ResilientSession::Impl::dial_once(const std::string& uri, DialResult* out,
                                       std::string* error) {
    std::unique_ptr<Transport> transport;
    if (!AdapterRegistry::instance().dial(uri, &transport, error)) return false;
    auto session = TunnelSession::create(std::move(transport), /*is_dialer=*/true,
                                         opts.tunnel_opts, error);
    if (!session) return false;
    auto closed = std::make_shared<std::atomic<bool>>(false);
    std::weak_ptr<std::atomic<bool>> wp(closed);
    if (logger_) session->set_logger(logger_);
    // close handler 在 start 之前装好；只置标记 + 唤醒，绝不调用本会话的
    // stop()（close handler 跑在隧道工作线程上，会自 join 死锁）
    session->set_close_handler([this, wp] {
        if (auto sp = wp.lock()) sp->store(true);
        // 不在此置 connected_=false：交给 loop 的 set_disconnected 统一处理
        cv.notify_all();
    });
    if (!session->start(error)) return false;
    if (!session->wait_hello(opts.connect_timeout_ms, error)) {
        session->stop();
        return false;
    }
    out->session = std::move(session);
    out->closed = std::move(closed);
    return true;
}

void ResilientSession::Impl::reopen(const std::shared_ptr<TunnelSession>& session) {
    // 此时 connected_ 为 false 且 current_ 为空，应用线程只能注册不能开通道，
    // 故无并发开通道竞态；逐通道快照后锁外阻塞打开
    for (size_t idx = 0; idx < channels_.size(); ++idx) {
        if (stop_.load()) return;
        std::string kind;
        ChannelMode mode = ChannelMode::Stream;
        DataHandler handler;
        {
            std::lock_guard<std::mutex> l(mu);
            if (channels_[idx].closed) continue;  // 应用已关闭的通道不再重开
            kind = channels_[idx].kind;
            mode = channels_[idx].mode;
            handler = channels_[idx].handler;
        }
        std::string err;
        const uint32_t wire =
            session->open_channel(kind, mode, opts.connect_timeout_ms, &err);
        if (wire == 0) {
            KOP_LOG_WARN(kTag, "重连后重开通道 [%s] 失败：%s", kind.c_str(), err.c_str());
            continue;
        }
        // 处理器随重连重装（打开成功后立即装，早于该通道任何数据帧投递）
        if (handler) session->set_data_handler(wire, handler);
        std::lock_guard<std::mutex> l(mu);
        channels_[idx].wire_id = wire;
    }
}

bool ResilientSession::Impl::set_connected(DialResult dr, uint64_t epoch) {
    {
        std::lock_guard<std::mutex> l(mu);
        if (dr.closed->load()) return false;  // 握手完成但传输已断：不宣布恢复
        if (epoch_ != epoch) return false;    // 已被更新的连接取代
        current_ = dr.session;
        session_closed_ = dr.closed;
        connected_ = true;
    }
    cv.notify_all();
    if (state_handler_) state_handler_(true);
    // 通道就绪回调：快照后锁外调用（回调里应用通常会装数据处理器/重发状态）
    struct Ready {
        uint32_t logical;
        std::string kind;
        ChannelMode mode;
    };
    std::vector<Ready> ready;
    {
        std::lock_guard<std::mutex> l(mu);
        ready.reserve(channels_.size());
        for (size_t i = 0; i < channels_.size(); ++i) {
            if (channels_[i].wire_id != 0) {
                ready.push_back(Ready{static_cast<uint32_t>(i + 1), channels_[i].kind,
                                      channels_[i].mode});
            }
        }
    }
    if (channel_handler_) {
        for (const auto& r : ready) channel_handler_(r.logical, r.kind, r.mode);
    }
    return true;
}

void ResilientSession::Impl::set_disconnected() {
    std::shared_ptr<TunnelSession> dead;
    bool was_connected = false;
    {
        std::lock_guard<std::mutex> l(mu);
        was_connected = connected_;
        connected_ = false;
        dead = std::move(current_);
        session_closed_.reset();
        for (auto& c : channels_) c.wire_id = 0;
    }
    cv.notify_all();
    // 锁外析构：~TunnelSession → stop → join 隧道工作线程，其退出路径会
    // 调 close handler（它要加 mu），故绝不能在持锁时析构
    dead.reset();
    if (was_connected && state_handler_) state_handler_(false);
}

void ResilientSession::Impl::notify_dead() {
    // 永久终止：先置 worker_done_ 再回调，回调方可据此区分“断开”与“已死”
    worker_done_.store(true);
    cv.notify_all();
    if (state_handler_) state_handler_(false);
    fire_close();
}

void ResilientSession::Impl::fire_close() {
    bool expected = false;
    if (!close_fired_.compare_exchange_strong(expected, true)) return;
    if (close_handler_) close_handler_();
}

void ResilientSession::Impl::loop() {
    uint32_t attempt = 0;
    while (!stop_.load()) {
        if (!sleep_interruptible(backoff(attempt))) break;
        if (stop_.load()) break;

        DialResult dr{nullptr, nullptr};
        std::string last_error;
        bool got = false;
        for (const auto& uri : opts.endpoints) {
            if (stop_.load()) break;
            std::string err;
            if (dial_once(uri, &dr, &err)) {
                got = true;
                break;
            }
            last_error = err;
        }
        if (stop_.load()) break;
        if (!got) {
            ++attempt;
            if (opts.max_attempts > 0 && attempt > opts.max_attempts) {
                KOP_LOG_ERROR(kTag, "重连次数耗尽（%u 次），会话停止", opts.max_attempts);
                notify_dead();
                break;
            }
            KOP_LOG_WARN(kTag, "全部 Endpoint 拨号失败（最近：%s），%ums 后重试",
                         last_error.c_str(), backoff(attempt + 1));
            continue;
        }
        attempt = 0;
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> l(mu);
            epoch = ++epoch_;
        }
        reopen(dr.session);
        if (!set_connected(std::move(dr), epoch)) {
            // 刚连上就断了：直接拆掉重试（set_disconnected 会清状态）
            set_disconnected();
            continue;
        }
        // 等传输断开或停机
        {
            std::unique_lock<std::mutex> l(mu);
            cv.wait(l, [&] {
                if (stop_.load()) return true;
                if (!connected_) return true;
                return session_closed_ && session_closed_->load();
            });
        }
        set_disconnected();
    }
    worker_done_.store(true);
    cv.notify_all();
}

ResilientSession::ResilientSession() : impl_(std::make_unique<Impl>()) {}

ResilientSession::ResilientSession(const Options& opts) : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;
}

void ResilientSession::set_options(Options opts) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->opts = std::move(opts);
}

ResilientSession::~ResilientSession() { stop(); }

bool ResilientSession::start(std::string* error) {
    if (impl_->started_.exchange(true)) {
        if (error) *error = "会话已启动";
        return false;
    }
    if (impl_->opts.endpoints.empty()) {
        if (error) *error = "endpoints 列表为空";
        impl_->started_.store(false);
        return false;
    }
    impl_->stop_.store(false);
    impl_->worker_ = std::thread([this] { impl_->loop(); });
    return true;
}

void ResilientSession::stop() {
    if (!impl_->started_.exchange(false)) return;
    impl_->stop_.store(true);
    impl_->cv.notify_all();
    if (impl_->worker_.joinable()) impl_->worker_.join();
    impl_->fire_close();
    // 兜底：连接线程可能在拨号途中被 stop 唤醒退出，其局部会话随栈展开拆除；
    // 这里再确保 current_ 被回收（锁外析构，理由同 set_disconnected）
    std::shared_ptr<TunnelSession> dead;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        dead = std::move(impl_->current_);
        impl_->connected_ = false;
        for (auto& c : impl_->channels_) c.wire_id = 0;
    }
    dead.reset();
}

uint32_t ResilientSession::open_channel(const std::string& kind, ChannelMode mode,
                                        std::string* error) {
    return open_channel(kind, mode,
                        static_cast<int>(impl_->opts.connect_timeout_ms), nullptr, error);
}

uint32_t ResilientSession::open_channel(const std::string& kind, ChannelMode mode,
                                        int timeout_ms, DataHandler handler,
                                        std::string* error) {
    std::shared_ptr<TunnelSession> snap;
    uint64_t epoch = 0;
    uint32_t logical = 0;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        impl_->channels_.push_back({kind, mode, 0, false, handler});
        logical = static_cast<uint32_t>(impl_->channels_.size());
        snap = impl_->current_;
        epoch = impl_->epoch_;
    }
    bool opened = false;
    if (snap) {
        std::string err;
        const uint32_t wire = snap->open_channel(kind, mode, timeout_ms, &err);
        if (wire != 0) {
            if (handler) snap->set_data_handler(wire, handler);
            std::lock_guard<std::mutex> l(impl_->mu);
            // epoch 未变才采用：否则这是已淘汰会话上的通道号，连接线程会
            // 在新会话上重开
            if (impl_->epoch_ == epoch) {
                impl_->channels_[logical - 1].wire_id = wire;
                opened = true;
            }
        } else if (error) {
            *error = err;
        }
    }
    // snap 在此析构（锁外，可能触发 ~TunnelSession）
    if (opened && impl_->channel_handler_) {
        impl_->channel_handler_(logical, kind, mode);
    }
    return logical;
}

void ResilientSession::set_data_handler(uint32_t logical, DataHandler handler) {
    std::shared_ptr<TunnelSession> snap;
    uint32_t wire = 0;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        if (logical == 0 || logical > impl_->channels_.size()) return;
        impl_->channels_[logical - 1].handler = handler;
        snap = impl_->current_;
        wire = impl_->channels_[logical - 1].wire_id;
    }
    if (snap && wire != 0 && handler) snap->set_data_handler(wire, handler);
}

void ResilientSession::close_channel(uint32_t logical) {
    std::shared_ptr<TunnelSession> snap;
    uint32_t wire = 0;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        if (logical == 0 || logical > impl_->channels_.size()) return;
        snap = impl_->current_;
        wire = impl_->channels_[logical - 1].wire_id;
        impl_->channels_[logical - 1].closed = true;
        impl_->channels_[logical - 1].wire_id = 0;
    }
    // 锁外关闭线上通道（close_channel 内部自带锁，顺序恒为 本锁→隧道锁）
    if (snap && wire != 0) snap->close_channel(wire);
}

void ResilientSession::set_channel_handler(ChannelHandler h) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->channel_handler_ = std::move(h);
}

void ResilientSession::set_state_handler(StateHandler h) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->state_handler_ = std::move(h);
}

void ResilientSession::set_close_handler(CloseHandler h) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->close_handler_ = std::move(h);
}

void ResilientSession::set_logger(TunnelLogger h) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->logger_ = std::move(h);
}

SendStatus ResilientSession::send(uint32_t logical, const uint8_t* data, size_t len,
                                  const std::vector<int>* fds, int timeout_ms) {
    std::shared_ptr<TunnelSession> snap;
    uint32_t wire = 0;
    {
        std::lock_guard<std::mutex> l(impl_->mu);
        if (!impl_->connected_) return SendStatus::Closed;
        if (!impl_->channel_lookup(logical, &wire)) return SendStatus::Closed;
        snap = impl_->current_;
    }
    if (!snap || wire == 0) return SendStatus::Closed;
    return snap->send(wire, data, len, fds, timeout_ms);
}

RecvStatus ResilientSession::recv(uint32_t logical, std::vector<uint8_t>* data,
                                  std::vector<int>* fds, uint32_t timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        std::shared_ptr<TunnelSession> snap;
        uint32_t wire = 0;
        uint64_t epoch = 0;
        {
            std::unique_lock<std::mutex> l(impl_->mu);
            impl_->cv.wait_until(l, deadline, [&] {
                if (impl_->stop_.load()) return true;
                uint32_t w = 0;
                if (!impl_->channel_lookup(logical, &w)) return true;
                return impl_->connected_ && w != 0;
            });
            if (impl_->stop_.load()) return RecvStatus::Closed;
            if (!impl_->channel_lookup(logical, &wire)) return RecvStatus::Closed;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return RecvStatus::Timeout;
            snap = impl_->current_;
            epoch = impl_->epoch_;
        }
        if (!snap || wire == 0) return RecvStatus::Closed;
        const auto now = std::chrono::steady_clock::now();
        int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (left <= 0) left = 1;
        const RecvStatus st = snap->recv(wire, data, fds, left);
        if (st != RecvStatus::Closed) return st;
        // Closed 两种来源：对端关通道（会话还活着）→ 返回；传输断了 → 等重连
        bool still_current = false;
        {
            std::lock_guard<std::mutex> l(impl_->mu);
            still_current = (impl_->epoch_ == epoch) && (impl_->current_ == snap);
        }
        if (still_current && snap->alive()) return RecvStatus::Closed;
        // 否则循环：park 等待新连接（wait_until 的 deadline 已逼近时会 Timeout）
    }
}

bool ResilientSession::connected() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    return impl_->connected_;
}

bool ResilientSession::reconnect_done() const {
    return impl_->worker_done_.load();
}

bool ResilientSession::supports_fds() const {
    std::lock_guard<std::mutex> l(impl_->mu);
    return impl_->connected_ && impl_->current_ &&
           impl_->current_->transport()->supports_fds();
}

bool ResilientSession::wait_connected(int timeout_ms) {
    std::unique_lock<std::mutex> l(impl_->mu);
    impl_->cv.wait_for(l, std::chrono::milliseconds(timeout_ms),
                       [&] { return impl_->connected_ || impl_->stop_.load(); });
    return impl_->connected_;
}

}  // namespace kopnet
