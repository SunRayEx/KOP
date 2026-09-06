//! 工作窃取调度器（P2）：全局注入队列 + 每工作者本地队列 + 跨工作者窃取。
//!
//! 外部提交（引擎 emit 路径）进入全局注入队列；工作者按 本地 → 全局 → 窃取
//! 的顺序取任务。单个响应式节点的 drain 严格串行（由 NodeExec.has_worker
//! 原子认领保证），不同节点并行；工作者数 < 节点数时形成 M:N 调度。
//! 本地队列供未来任务内派生子任务使用（当前由窃取路径覆盖）。

use crossbeam_deque::{Injector, Stealer, Worker};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

pub type Job = Box<dyn FnOnce() + Send + 'static>;

pub struct Scheduler {
    global: Injector<Job>,
    stop: Arc<AtomicBool>,
    handles: Mutex<Vec<JoinHandle<()>>>,
    stopped_flag: AtomicBool,
}

impl Scheduler {
    /// 启动 n 个工作者线程。
    pub fn new(n: usize) -> Arc<Self> {
        let global = Injector::new();
        let stop = Arc::new(AtomicBool::new(false));
        let mut handles = Vec::with_capacity(n);
        // 每工作者一个本地队列（先建好再取 stealer，然后移动进线程）
        let mut local_workers: Vec<Worker<Job>> = (0..n).map(|_| Worker::new_fifo()).collect();
        let stealers: Vec<Stealer<Job>> =
            local_workers.iter().map(|w| w.stealer()).collect();

        let sched = Arc::new(Scheduler {
            global,
            stop,
            handles: Mutex::new(Vec::new()),
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

    pub fn submit(&self, job: Job) {
        self.global.push(job);
    }

    /// 请求停止：工作者退出前排空全局队列，保证已提交任务执行完毕。
    pub fn shutdown(&self) {
        if self.stopped_flag.swap(true, Ordering::AcqRel) {
            return;
        }
        self.stop.store(true, Ordering::Release);
        let handles: Vec<_> =
            self.handles.lock().unwrap_or_else(|p| p.into_inner()).drain(..).collect();
        for h in handles {
            let _ = h.join();
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
