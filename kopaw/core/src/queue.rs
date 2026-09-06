//! 有界帧队列：跨线程搬运 `*mut KopawFrame`（独占所有权转移）。
//!
//! - 发送：阻塞式背压；图停止时释放滞留帧并返回 STOPPED；
//! - 接收：带超时轮询，保证停止标志能在 1ms 内被观察到；
//! - 停止/析构时队列内残留帧由 `drain_release` 统一归还生产者，杜绝泄漏。

use crate::ffi::KopawFrame;
use crossbeam_channel::{RecvTimeoutError, TrySendError};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

/// 安全地把裸帧指针跨线程搬运。
/// 帧指针的所有权语义由引擎单线程顺序保证：任一时刻至多一处持有。
struct SendFrame(*mut KopawFrame);
unsafe impl Send for SendFrame {}

/// 复用图状态常量：2 = STOPPING（与 ffi::KOPAW_STATE_STOPPING 一致）
const STATE_STOPPING: u8 = 2;

pub enum RecvResult {
    /// 调用方获得帧所有权（可能带 EOS 标志）
    Frame(*mut KopawFrame),
    Timeout,
    Stopped,
}

pub enum SendResult {
    Ok,
    /// 图停止：帧已被引擎释放，调用方不得再触碰
    Stopped,
}

/// 归还一帧给其生产者（调用 release 回调；引用计数减一，归零才真正释放）。
/// 引擎与队列在所有非消费路径上都通过它释放帧。
pub(crate) unsafe fn release_frame(f: *mut KopawFrame) {
    if f.is_null() {
        return;
    }
    if let Some(rel) = (*f).release {
        rel(f);
    }
}

/// 增加一个引用（tee/多出边向后续链路投递时调用）。
pub(crate) unsafe fn retain_frame(f: *mut KopawFrame) {
    if f.is_null() {
        return;
    }
    if let Some(ret) = (*f).retain {
        ret(f);
    }
}

use std::sync::atomic::AtomicU64;

/// 链路占用统计（无锁计数；max_occ 为近似峰值）
#[derive(Default)]
pub(crate) struct QueueStats {
    pub enqueued: AtomicU64,
    pub dequeued: AtomicU64,
    pub max_occ: AtomicU64,
}

pub(crate) struct FrameQueue {
    cap: usize,
    tx: crossbeam_channel::Sender<SendFrame>,
    rx: crossbeam_channel::Receiver<SendFrame>,
    state: Arc<AtomicU8>,
    pub stats: QueueStats,
}

impl FrameQueue {
    pub fn new(cap: u32, state: Arc<AtomicU8>) -> Self {
        let (tx, rx) = crossbeam_channel::bounded(cap.max(1) as usize);
        FrameQueue { cap: cap.max(1) as usize, tx, rx, state, stats: QueueStats::default() }
    }

    fn note_enqueue(&self) {
        use std::sync::atomic::Ordering;
        let enq = self.stats.enqueued.fetch_add(1, Ordering::Relaxed) + 1;
        let occ = enq - self.stats.dequeued.load(Ordering::Relaxed);
        self.stats.max_occ.fetch_max(occ, Ordering::Relaxed);
    }

    fn note_dequeue(&self) {
        use std::sync::atomic::Ordering;
        self.stats.dequeued.fetch_add(1, Ordering::Relaxed);
    }

    pub fn capacity(&self) -> usize {
        self.cap
    }

    /// 阻塞发送：队列满时自旋等待，观察停止标志。
    pub fn send(&self, frame: *mut KopawFrame) -> SendResult {
        loop {
            if self.state.load(Ordering::Acquire) >= STATE_STOPPING {
                unsafe { release_frame(frame) };
                return SendResult::Stopped;
            }
            match self.tx.try_send(SendFrame(frame)) {
                Ok(()) => {
                    self.note_enqueue();
                    return SendResult::Ok;
                }
                Err(TrySendError::Full(_)) => thread::sleep(Duration::from_millis(1)),
                Err(TrySendError::Disconnected(_)) => {
                    unsafe { release_frame(frame) };
                    return SendResult::Stopped;
                }
            }
        }
    }

    /// 带超时接收：供引擎响应式节点循环与自驱动节点使用。
    pub fn recv(&self, timeout: Duration) -> RecvResult {
        let deadline_poll = Duration::from_millis(1);
        let mut waited = Duration::ZERO;
        loop {
            if self.state.load(Ordering::Acquire) >= STATE_STOPPING {
                return RecvResult::Stopped;
            }
            match self.rx.recv_timeout(deadline_poll) {
                Ok(SendFrame(f)) => {
                    self.note_dequeue();
                    return RecvResult::Frame(f);
                }
                Err(RecvTimeoutError::Timeout) => {
                    waited += deadline_poll;
                    if timeout != Duration::ZERO && waited >= timeout {
                        return RecvResult::Timeout;
                    }
                }
                Err(RecvTimeoutError::Disconnected) => return RecvResult::Stopped,
            }
        }
    }

    /// 立即取走一帧（无等待）；自驱动节点在无新帧时用于保持渲染循环。
    #[allow(dead_code)] // 引擎 API 预留（非自驱动的轮询场景）
    pub fn try_recv(&self) -> Option<*mut KopawFrame> {
        self.rx.try_recv().ok().map(|SendFrame(f)| f)
    }

    /// 唤醒阻塞在发送上的生产者：crossbeam 通道无显式唤醒原语，
    /// send 的自旋会自行观察 state 标志，此函数仅作语义占位。
    #[allow(dead_code)]
    pub fn wake_senders(&self) {}

    /// 停止后清空并释放全部滞留帧（只允许在所有线程退出后调用）。
    pub fn drain_release(&self) {
        while let Ok(SendFrame(f)) = self.rx.try_recv() {
            unsafe { release_frame(f) };
        }
    }
}

impl std::fmt::Debug for FrameQueue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "FrameQueue(cap={})", self.cap)
    }
}
