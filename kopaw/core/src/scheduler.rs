//! 工作窃取调度器（P2）：全局注入队列 + 每工作者本地队列 + 跨工作者窃取。
//!
//! 外部提交（引擎 emit 路径）进入全局注入队列；工作者按 本地 → 全局 → 窃取
//! 的顺序取任务。单个响应式节点的 drain 严格串行（由 NodeExec.has_worker
//! 原子认领保证），不同节点并行；工作者数 < 节点数时形成 M:N 调度。
//! 本地队列供未来任务内派生子任务使用（当前由窃取路径覆盖）。

use crossbeam_deque::{Injector, Stealer, Worker};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle, ThreadId};
use std::time::Duration;

pub type Job = Box<dyn FnOnce() + Send + 'static>;

pub struct Scheduler {
    global: Injector<Job>,
    stop: Arc<AtomicBool>,
    /// Serializes the admission check and the Injector push. Shutdown holds
    /// this briefly while closing admission, so no job can be pushed after
    /// workers have been told to drain and exit.
    admission: Mutex<()>,
    handles: Mutex<Vec<JoinHandle<()>>>,
    /// Serialize shutdown itself. A worker may call shutdown from a job; the
    /// current worker handle is then retained for a later external join.
    shutdown_lock: Mutex<()>,
    stopped_flag: AtomicBool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Barrier;

    #[test]
    fn shutdown_executes_every_accepted_job() {
        let scheduler = Scheduler::new(2);
        let count = Arc::new(AtomicUsize::new(0));
        let mut accepted = 0;
        for _ in 0..64 {
            let count = count.clone();
            if scheduler.submit(Box::new(move || {
                count.fetch_add(1, Ordering::SeqCst);
            })) {
                accepted += 1;
            }
        }
        scheduler.shutdown();
        assert_eq!(count.load(Ordering::SeqCst), accepted);
        assert!(!scheduler.submit(Box::new(|| {})));
    }

    #[test]
    fn worker_shutdown_does_not_join_itself() {
        let scheduler = Scheduler::new(1);
        let done = Arc::new(AtomicBool::new(false));
        let done2 = done.clone();
        let scheduler2 = scheduler.clone();
        assert!(scheduler.submit(Box::new(move || {
            scheduler2.shutdown();
            done2.store(true, Ordering::SeqCst);
        })));
        for _ in 0..100 {
            if done.load(Ordering::SeqCst) {
                break;
            }
            thread::sleep(Duration::from_millis(1));
        }
        assert!(done.load(Ordering::SeqCst));
        scheduler.shutdown();
    }

    #[test]
    fn zero_workers_are_normalized() {
        let scheduler = Scheduler::new(0);
        assert_eq!(scheduler.worker_count(), 1);
        let done = Arc::new(AtomicBool::new(false));
        let done2 = done.clone();
        assert!(scheduler.submit(Box::new(move || {
            done2.store(true, Ordering::SeqCst);
        })));
        for _ in 0..100 {
            if done.load(Ordering::SeqCst) {
                break;
            }
            thread::sleep(Duration::from_millis(1));
        }
        assert!(done.load(Ordering::SeqCst));
        scheduler.shutdown();
    }

    #[test]
    fn many_threads_can_shutdown_idempotently() {
        let scheduler = Scheduler::new(4);
        let barrier = Arc::new(Barrier::new(8));
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let scheduler = scheduler.clone();
                let barrier = barrier.clone();
                scope.spawn(move || {
                    barrier.wait();
                    scheduler.shutdown();
                });
            }
        });
        assert_eq!(scheduler.worker_count(), 0);
        assert!(!scheduler.submit(Box::new(|| {})));
        scheduler.shutdown();
    }

    #[test]
    fn worker_can_submit_before_shutdown() {
        let scheduler = Scheduler::new(2);
        let done = Arc::new(AtomicUsize::new(0));
        let child_done = done.clone();
        let scheduler2 = scheduler.clone();
        assert!(scheduler.submit(Box::new(move || {
            child_done.fetch_add(1, Ordering::SeqCst);
            assert!(scheduler2.submit(Box::new(|| {})));
        })));
        for _ in 0..100 {
            if done.load(Ordering::SeqCst) == 1 {
                break;
            }
            thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(done.load(Ordering::SeqCst), 1);
        scheduler.shutdown();
    }

    #[test]
    fn shutdown_and_submit_have_single_admission_boundary() {
        let scheduler = Scheduler::new(2);
        let barrier = Arc::new(Barrier::new(2));
        let accepted = Arc::new(AtomicUsize::new(0));
        let s2 = scheduler.clone();
        let b2 = barrier.clone();
        let a2 = accepted.clone();
        let submitter = thread::spawn(move || {
            b2.wait();
            for _ in 0..1000 {
                if s2.submit(Box::new(|| {})) {
                    a2.fetch_add(1, Ordering::SeqCst);
                }
            }
        });
        barrier.wait();
        scheduler.shutdown();
        submitter.join().unwrap();
        assert!(!scheduler.submit(Box::new(|| {})));
        let _ = accepted;
    }
}

impl Scheduler {
    /// 启动 n 个工作者线程。
    pub fn new(n: usize) -> Arc<Self> {
        // A zero-worker pool cannot make progress; retain the public API's
        // usable invariant by creating one worker.
        let n = n.max(1);
        let global = Injector::new();
        let stop = Arc::new(AtomicBool::new(false));
        let mut handles = Vec::with_capacity(n);
        // 每工作者一个本地队列（先建好再取 stealer，然后移动进线程）
        let local_workers: Vec<Worker<Job>> = (0..n).map(|_| Worker::new_fifo()).collect();
        let stealers: Vec<Stealer<Job>> = local_workers.iter().map(|w| w.stealer()).collect();

        let sched = Arc::new(Scheduler {
            global,
            stop,
            admission: Mutex::new(()),
            handles: Mutex::new(Vec::new()),
            shutdown_lock: Mutex::new(()),
            stopped_flag: AtomicBool::new(false),
        });

        for (i, w) in local_workers.into_iter().enumerate() {
            let sched2 = sched.clone();
            let stealers = stealers.clone();
            let h = std::thread::Builder::new()
                .name(format!("kopaw:w{i}"))
                .spawn(move || {
                    worker_loop(w, &sched2, &stealers);
                });
            if let Ok(h) = h {
                handles.push(h);
            }
        }
        *sched.handles.lock().unwrap_or_else(|p| p.into_inner()) = handles;
        sched
    }

    /// Submit a job if shutdown has not closed admission. The return value is
    /// false when the caller owns a frame queued in a graph execution body
    /// but no worker can accept another drain task; the graph then releases
    /// that body explicitly instead of leaving a task stranded in Injector.
    pub fn submit(&self, job: Job) -> bool {
        let _admission = self.admission.lock().unwrap_or_else(|p| p.into_inner());
        if self.stopped_flag.load(Ordering::Acquire) {
            return false;
        }
        self.global.push(job);
        true
    }

    /// Close job admission and request worker exit. Already admitted jobs are
    /// still drained by workers before they return. This method deliberately
    /// does not join, so a graph node can request shutdown from an event
    /// callback without trying to join its own worker thread.
    pub fn request_stop(&self) {
        let _admission = self.admission.lock().unwrap_or_else(|p| p.into_inner());
        if !self.stopped_flag.swap(true, Ordering::AcqRel) {
            self.stop.store(true, Ordering::Release);
        }
    }

    /// Request stop and join all workers except the caller. If called by a
    /// worker job, retain that worker's JoinHandle so a later external call
    /// can join it after the job returns.
    pub fn shutdown(&self) {
        let current = thread::current().id();
        let handles = {
            let _shutdown = self.shutdown_lock.lock().unwrap_or_else(|p| p.into_inner());
            self.request_stop();

            // A worker cannot join itself. Leave all handles in place; an
            // external caller can join them after this callback returns.
            let mut all = self.handles.lock().unwrap_or_else(|p| p.into_inner());
            if all.iter().any(|h| h.thread().id() == current) {
                return;
            }
            all.drain(..).collect::<Vec<_>>()
        };
        // Do not hold shutdown_lock while joining. A job may call shutdown
        // while another thread is waiting for that job to return.
        for h in handles {
            let _ = h.join();
        }
    }

    pub fn worker_count(&self) -> usize {
        self.handles.lock().unwrap_or_else(|p| p.into_inner()).len()
    }

    pub fn is_worker_thread(&self, id: ThreadId) -> bool {
        self.handles
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .iter()
            .any(|h| h.thread().id() == id)
    }
}

impl Drop for Scheduler {
    fn drop(&mut self) {
        self.request_stop();
        let current = thread::current().id();
        let mut handles = self.handles.lock().unwrap_or_else(|p| p.into_inner());
        let remaining = handles.drain(..).collect::<Vec<_>>();
        drop(handles);
        for handle in remaining {
            if handle.thread().id() != current {
                let _ = handle.join();
            }
        }
    }
}

fn worker_loop(local: Worker<Job>, sched: &Scheduler, stealers: &[Stealer<Job>]) {
    let stop = &sched.stop;
    let global = &sched.global;
    loop {
        if stop.load(Ordering::Acquire) {
            // 排空后退出（保证已提交任务执行完毕）
            while let Some(j) = local.pop() {
                j();
            }
            loop {
                match global.steal() {
                    crossbeam_deque::Steal::Success(j) => j(),
                    crossbeam_deque::Steal::Empty => return,
                    crossbeam_deque::Steal::Retry => continue,
                }
            }
        }
        // 1) 本地（任务派生场景）→ 2) 全局窃取 → 3) 窃取他人
        if let Some(j) = local.pop() {
            j();
            continue;
        }
        let mut got = None;
        loop {
            match global.steal() {
                crossbeam_deque::Steal::Success(j) => {
                    got = Some(j);
                    break;
                }
                crossbeam_deque::Steal::Empty => break,
                crossbeam_deque::Steal::Retry => continue,
            }
        }
        if let Some(j) = got {
            j();
            continue;
        }
        let mut stolen = false;
        for st in stealers {
            loop {
                match st.steal() {
                    crossbeam_deque::Steal::Success(j) => {
                        j();
                        stolen = true;
                        break;
                    }
                    crossbeam_deque::Steal::Empty => break,
                    crossbeam_deque::Steal::Retry => continue,
                }
            }
            if stolen {
                break;
            }
        }
        if !stolen {
            std::thread::sleep(Duration::from_micros(200));
        }
    }
}
