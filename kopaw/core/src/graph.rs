//! 图引擎：节点注册、链路连接、线程调度、EOS 记账与停止/析构语义。
//!
//! 并发模型（MVP，为后续工作窃取调度预留演进空间）：
//! - 每节点至多一线程；三种节点形态：
//!   · 源节点（无入边）：线程运行 `vtable.run`，主动生产；
//!   · 响应式节点（有入边且非自驱动）：引擎线程循环从入边队列取帧调 `vtable.send`；
//!   · 自驱动节点（self_driven）：线程运行 `vtable.run`，节点用 kopaw_node_recv 拉取；
//! - 所有队列统一观察图状态：STOPPING 及之后一切终态都会解除阻塞；
//! - 线程全部退出后，队列滞留帧统一归还生产者，再调用节点 destroy。
//!
//! 安全说明：GraphCore 的内部可变性全部处于 Mutex/Atomic 之后；节点 user
//! 指针的生命周期由 ABI 契约保证（graph_free 返回前 C++ 不得释放节点对象）。
//! 因此这里手动实现 Send/Sync 是有依据的。

use crate::clock::MediaClock;
use crate::ffi::{self, KopawEventFn, KopawEventType, KopawFrame, KopawNodeVTable, KopawOutput};
use crate::queue::{release_frame, retain_frame, FrameQueue, RecvResult, SendResult};
use crate::scheduler::Scheduler;
use std::collections::{HashMap, VecDeque};
use std::os::raw::c_void;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

/// 每节点统计（性能基线）
#[derive(Default)]
pub(crate) struct NodeStat {
    /// 引擎交付给该节点的帧数（send/recv 合计）
    pub delivered: AtomicU64,
    /// 响应式节点 send 处理累计耗时（µs；自驱动节点不统计）
    pub busy_us: AtomicU64,
    /// 单帧最大处理耗时（µs）
    pub busy_max_us: AtomicU64,
}

pub(crate) struct GraphCore {
    inner: Mutex<Inner>,
    state: Arc<AtomicU8>,
    pub(crate) clock: MediaClock,
    event: Mutex<EventSlot>,
    threads: Mutex<Vec<std::thread::JoinHandle<()>>>,
    stop_hints_done: AtomicBool,
    /// 每节点统计（下标 = node_id - 1；仅在构建期追加）
    stats: Mutex<Vec<Arc<NodeStat>>>,
    /// emit 总次数（至少一条出边被投递）
    emitted_total: AtomicU64,
    /// 因输出未连接而被丢弃的帧数
    dropped_unconnected: AtomicU64,
    /// P2 池调度：worker 数（0 = 每节点一线程）；构建期设置
    workers: AtomicUsize,
    /// 池模式下响应式节点的执行体
    execs: Mutex<Vec<Arc<NodeExec>>>,
    sched: Mutex<Option<Arc<Scheduler>>>,
}

/// 免毒化互斥锁访问：锁中毒时直接取回内部数据继续工作。
fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    match m.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    }
}

struct EventSlot {
    cb: Option<KopawEventFn>,
    user: *mut c_void,
}

struct NodeSlot {
    name: String,
    user: *mut c_void,
    vt: KopawNodeVTable,
    /// 输出端口 → 出边链路 id 列表（P1：允许多条出边 = tee 广播）
    outputs: Vec<Vec<u64>>,
    /// 输入端口 → 入边链路 id（P2：自驱动节点可多入边；响应式节点固定 1）
    inputs: Vec<Option<u64>>,
    is_sink: bool,
    self_driven: bool,
    /// 声明的每条入边默认队列容量（connect 传 cap=0 时采用）
    queue_cap: u32,
    /// 节点统计（线程共享）
    stat: Arc<NodeStat>,
}

/// 池调度模式下响应式节点的执行体：有界待处理队列 + 串行认领。
pub(crate) struct NodeExec {
    node_id: u32,
    name: String,
    user: *mut c_void,
    vt: KopawNodeVTable,
    is_sink: bool,
    stat: Arc<NodeStat>,
    mx: Mutex<VecDeque<*mut KopawFrame>>,
    cv: Condvar,
    cap: usize,
    /// 是否已有 drain 任务在执行（单节点串行化）
    has_worker: AtomicBool,
    /// 停止后不再接收
    stopped: AtomicBool,
}

// user 指向的 C++ 节点按 ABI 契约在 graph_free 前有效且可跨线程调用；
// 其余字段为原子量/锁保护队列。Arc<NodeExec> 随任务跨线程移动是安全的。
unsafe impl Send for NodeExec {}
unsafe impl Sync for NodeExec {}

struct Link {
    src_node: u32,
    src_port: u32,
    dst_node: u32,
    queue: Arc<FrameQueue>,
}

struct Inner {
    next_node_id: u32,
    link_seq: u64,
    nodes: HashMap<u32, NodeSlot>,
    links: HashMap<u64, Link>,
    /// 输出端口 id → 出边链路 id 列表
    out_to_link: HashMap<u64, Vec<u64>>,
    started: bool,
    destroyed: bool,
    sink_total: usize,
    sink_done: usize,
}

unsafe impl Send for GraphCore {}
unsafe impl Sync for GraphCore {}

impl GraphCore {
    pub fn new() -> Self {
        GraphCore {
            inner: Mutex::new(Inner {
                next_node_id: 1,
                link_seq: 0,
                nodes: HashMap::new(),
                links: HashMap::new(),
                out_to_link: HashMap::new(),
                started: false,
                destroyed: false,
                sink_total: 0,
                sink_done: 0,
            }),
            state: Arc::new(AtomicU8::new(ffi::KOPAW_STATE_IDLE as u8)),
            clock: MediaClock::default(),
            event: Mutex::new(EventSlot { cb: None, user: std::ptr::null_mut() }),
            threads: Mutex::new(Vec::new()),
            stop_hints_done: AtomicBool::new(false),
            stats: Mutex::new(Vec::new()),
            emitted_total: AtomicU64::new(0),
            dropped_unconnected: AtomicU64::new(0),
            workers: AtomicUsize::new(0),
            execs: Mutex::new(Vec::new()),
            sched: Mutex::new(None),
        }
    }

    pub fn add_node(
        &self,
        name: String,
        user: *mut c_void,
        vt: &KopawNodeVTable,
        outputs: u32,
        inputs: u32,
        queue_capacity: u32,
        is_sink: bool,
        self_driven: bool,
    ) -> Result<u32, i32> {
        if outputs > 64
            || queue_capacity == 0
            || (vt.struct_size as usize) < ffi::KOPAW_NODE_VTABLE_BASE_SIZE
        {
            return Err(ffi::KOPAW_E_INVALID);
        }
        // 响应式节点由引擎按单入边驱动（多入边只允许自驱动节点声明）；
        // 0 入边的源节点合法（run 主动生产）。
        if !self_driven && inputs > 1 {
            return Err(ffi::KOPAW_E_INVALID);
        }
        let mut g = lock(&self.inner);
        if g.started {
            return Err(ffi::KOPAW_E_INVALID); // 运行期不允许改图
        }
        let id = g.next_node_id;
        g.next_node_id += 1;
        let stat = Arc::new(NodeStat::default());
        g.nodes.insert(
            id,
            NodeSlot {
                name,
                user,
                vt: *vt,
                outputs: vec![Vec::new(); outputs as usize],
                inputs: vec![None; inputs as usize],
                is_sink,
                self_driven,
                queue_cap: queue_capacity,
                stat: stat.clone(),
            },
        );
        lock(&self.stats).push(stat);
        Ok(id)
    }

    /// P2：设置池调度 worker 数（0 = 每节点一线程）；仅构建期有效。
    pub fn set_workers(&self, workers: u32) -> u32 {
        if workers == 0 {
            return 0;
        }
        self.workers.store(workers as usize, Ordering::Relaxed);
        workers
    }

    fn output_id(node: u32, port: u32) -> u64 {
        ((node as u64) << 32) | (port as u64)
    }

    pub fn node_output(&self, node: u32, port: u32) -> Option<KopawOutput> {
        let g = lock(&self.inner);
        let n = g.nodes.get(&node)?;
        if port as usize >= n.outputs.len() {
            return None;
        }
        Some(KopawOutput { id: Self::output_id(node, port) })
    }

    pub fn connect(&self, src: KopawOutput, dst: u32, dst_port: u32, cap: u32) -> i32 {
        let mut g = lock(&self.inner);
        if g.started {
            return ffi::KOPAW_E_INVALID;
        }
        let (src_node, src_port) = ((src.id >> 32) as u32, (src.id & 0xFFFF_FFFF) as u32);
        if src_node == dst {
            return ffi::KOPAW_E_INVALID; // 自环禁止
        }
        {
            let sn = match g.nodes.get(&src_node) {
                Some(n) => n,
                None => return ffi::KOPAW_E_INVALID,
            };
            if src_port as usize >= sn.outputs.len() {
                return ffi::KOPAW_E_INVALID;
            }
            // P1：同一输出端口允许多条出边（tee 广播），不再拒绝
        }
        let dn = match g.nodes.get_mut(&dst) {
            Some(n) => n,
            None => return ffi::KOPAW_E_INVALID,
        };
        if dst_port as usize >= dn.inputs.len() || dn.inputs[dst_port as usize].is_some() {
            return ffi::KOPAW_E_INVALID; // 每个输入端口至多一条入边
        }

        let link_id = {
            g.link_seq += 1;
            let lid = g.link_seq;
            // cap == 0 → 采用目标节点声明的默认容量
            let cap = if cap == 0 {
                g.nodes.get(&dst).map(|n| n.queue_cap).unwrap_or(4)
            } else {
                cap
            };
            g.links.insert(
                lid,
                Link {
                    src_node,
                    src_port,
                    dst_node: dst,
                    queue: Arc::new(FrameQueue::new(cap, self.state.clone())),
                },
            );
            lid
        };
        g.nodes.get_mut(&dst).unwrap().inputs[dst_port as usize] = Some(link_id);
        g.nodes.get_mut(&src_node).unwrap().outputs[src_port as usize].push(link_id);
        g.out_to_link.entry(src.id).or_default().push(link_id);
        ffi::KOPAW_OK
    }

    pub fn start(&self) -> i32 {
        let jobs: Vec<NodeJob>;
        {
            let mut g = lock(&self.inner);
            if g.started {
                return ffi::KOPAW_E_INVALID;
            }
            g.started = true;
            g.sink_total = g.nodes.values().filter(|n| n.is_sink).count();
            g.sink_done = 0;
            jobs = g
                .nodes
                .iter()
                .map(|(id, n)| NodeJob {
                    node: *id,
                    user: n.user,
                    vt: n.vt,
                    is_sink: n.is_sink,
                    self_driven: n.self_driven,
                    name: n.name.clone(),
                    stat: n.stat.clone(),
                    in_queue: n.inputs.first().copied().flatten().map(|lid| g.links[&lid].queue.clone()),
                })
                .collect();
        }

        if self
            .state
            .compare_exchange(
                ffi::KOPAW_STATE_IDLE as u8,
                ffi::KOPAW_STATE_RUNNING as u8,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            return ffi::KOPAW_E_INVALID;
        }

        // P2：workers>0 → 池调度；响应式节点不再各占一线程
        let workers = self.workers.load(Ordering::Relaxed);
        if workers > 0 {
            *lock(&self.sched) = Some(Scheduler::new(workers));
        }

        let mut handles = Vec::with_capacity(jobs.len());
        for job in jobs {
            let this = GraphRef(self);
            let kind = match (&job.in_queue, job.self_driven) {
                (None, _) => NodeKind::Source,
                (Some(_), true) => NodeKind::SelfDriven,
                (Some(_), false) => NodeKind::Reactive,
            };
            match kind {
                NodeKind::Reactive if workers > 0 => {
                    // 池模式：建执行体，不占线程
                    let cap = job.in_queue.as_ref().map(|q| q.capacity()).unwrap_or(4);
                    let exec = Arc::new(NodeExec {
                        node_id: job.node,
                        name: job.name.clone(),
                        user: job.user,
                        vt: job.vt,
                        is_sink: job.is_sink,
                        stat: job.stat.clone(),
                        mx: Mutex::new(VecDeque::new()),
                        cv: Condvar::new(),
                        cap,
                        has_worker: AtomicBool::new(false),
                        stopped: AtomicBool::new(false),
                    });
                    lock(&self.execs).push(exec);
                }
                NodeKind::Reactive => {
                    let q = job.in_queue.clone().expect("reactive has queue");
                    let h = std::thread::Builder::new()
                        .name(format!("kopaw:{}", job.name))
                        .spawn(move || Self::reactive_loop(&this, &job, &q));
                    if let Ok(h) = h {
                        handles.push(h);
                    }
                }
                _ => {
                    // 源节点 / 自驱动节点：专线程运行 run()
                    // 整体捕获 job（disjoint capture 会把 user 裸指针单独捕获）
                    let job2: NodeJob = job;
                    let h = std::thread::Builder::new()
                        .name(format!("kopaw:{}", job2.name))
                        .spawn(move || {
                            let job = &job2;
                            if let Some(run) = job.vt.run {
                                let rc = unsafe { run(job.user) };
                                if rc != ffi::KOPAW_OK {
                                    unsafe { this.report_error(&job.name, rc) };
                                }
                            }
                        });
                    if let Ok(h) = h {
                        handles.push(h);
                    }
                }
            }
        }
        lock(&self.threads).extend(handles);
        ffi::KOPAW_OK
    }

    /// 响应式消费循环：引擎代为拉取并调用节点的 send，同时记录节点耗时统计。
    fn reactive_loop(this: &GraphRef, job: &NodeJob, queue: &Arc<FrameQueue>) {
        loop {
            match queue.recv(Duration::from_millis(2)) {
                RecvResult::Stopped => break,
                RecvResult::Timeout => continue,
                RecvResult::Frame(f) => {
                    let had_eos = unsafe { (*f).flags & ffi::KOPAW_FRAME_FLAG_EOS != 0 };
                    let t0 = Instant::now();
                    let rc = match job.vt.send {
                        Some(send) => unsafe { send(job.user, f) },
                        None => ffi::KOPAW_E_GENERIC,
                    };
                    let us = t0.elapsed().as_micros() as u64;
                    job.stat.delivered.fetch_add(1, Ordering::Relaxed);
                    job.stat.busy_us.fetch_add(us, Ordering::Relaxed);
                    job.stat.busy_max_us.fetch_max(us, Ordering::Relaxed);
                    if rc == ffi::KOPAW_OK {
                        if had_eos && job.is_sink {
                            // P1 延迟记账：引擎不再自动计数；汇聚节点在真正
                            // 完成（如音频环形缓冲排空）后经
                            // kopaw_node_sink_done 自行记账。
                            break;
                        }
                        // 非汇聚节点：EOS 由 C++ 侧经 emit 向下游转发
                    } else {
                        // 节点未接管帧，引擎归还生产者
                        unsafe { release_frame(f) };
                        unsafe { this.report_error(&job.name, rc) };
                        break;
                    }
                }
            }
        }
    }

    pub fn stop(&self) -> i32 {
        // 1) 状态推进到 STOPPING（不回退既有终态）
        let cur = self.state.load(Ordering::Acquire);
        if (cur as i32) < ffi::KOPAW_STATE_STOPPING {
            self.state.store(ffi::KOPAW_STATE_STOPPING as u8, Ordering::Release);
        }
        // 2) 唤醒池模式执行体（等待中的 push/drain 立即退出）
        for e in lock(&self.execs).iter() {
            e.stopped.store(true, Ordering::Release);
            e.cv.notify_all();
        }
        // 3) 一次性发送节点级停止提示
        if !self.stop_hints_done.swap(true, Ordering::AcqRel) {
            let g = lock(&self.inner);
            for n in g.nodes.values() {
                if let Some(stop) = n.vt.stop {
                    unsafe { stop(n.user) };
                }
            }
        }
        // 4) 等待全部线程退出（节点线程 + 调度器工作者）
        let handles: Vec<_> = lock(&self.threads).drain(..).collect();
        for h in handles {
            let _ = h.join();
        }
        if let Some(s) = lock(&self.sched).take() {
            s.shutdown();
        }
        // 5) 回收滞留帧（线程已全部退出，此处无竞争）
        {
            let g = lock(&self.inner);
            for l in g.links.values() {
                l.queue.drain_release();
            }
        }
        for e in lock(&self.execs).iter() {
            let mut st = lock(&e.mx);
            for f in st.drain(..) {
                unsafe { release_frame(f) };
            }
            e.cv.notify_all();
        }
        ffi::KOPAW_OK
    }

    /// free 路径：停止 → 回收 → 逐节点 destroy。
    pub fn shutdown(&self) {
        self.stop();
        let mut g = lock(&self.inner);
        if !g.destroyed {
            g.destroyed = true;
            for n in g.nodes.values() {
                if let Some(destroy) = n.vt.destroy {
                    unsafe { destroy(n.user) };
                }
            }
        }
    }

    pub fn set_event_cb(&self, cb: Option<KopawEventFn>, user: *mut c_void) {
        let mut e = lock(&self.event);
        e.cb = cb;
        e.user = user;
    }

    pub fn state(&self) -> i32 {
        self.state.load(Ordering::Acquire) as i32
    }

    /// P1 多出边：向端口的全部出边广播（阻塞式背压，慢消费者拖住整体节奏）。
    /// 帧经引用计数共享：投递给第 i 条边（i>0）前先 retain；
    /// 图停止时由 queue.send 归还当前引用并返回 STOPPED。
    pub fn emit(&self, out: KopawOutput, frame: *mut KopawFrame) -> i32 {
        // 解析出边目标：池模式下响应式目标走 NodeExec，其余走有界队列
        enum Target {
            Queue(Arc<FrameQueue>),
            Exec(Arc<NodeExec>),
        }
        let targets: Vec<Target> = {
            let g = lock(&self.inner);
            let workers = self.workers.load(Ordering::Relaxed);
            let execs = lock(&self.execs);
            match g.out_to_link.get(&out.id) {
                Some(lids) if !lids.is_empty() => lids
                    .iter()
                    .map(|lid| {
                        let l = &g.links[lid];
                        if workers > 0 {
                            if let Some(e) = execs.iter().find(|e| e.node_id == l.dst_node) {
                                return Target::Exec(e.clone());
                            }
                        }
                        Target::Queue(l.queue.clone())
                    })
                    .collect(),
                _ => {
                    self.dropped_unconnected.fetch_add(1, Ordering::Relaxed);
                    unsafe { release_frame(frame) };
                    return ffi::KOPAW_OK;
                }
            }
        };
        // tee 前提：帧必须支持引用计数
        if targets.len() > 1 && unsafe { (*frame).retain.is_none() } {
            self.dropped_unconnected.fetch_add(1, Ordering::Relaxed);
            unsafe { release_frame(frame) };
            return ffi::KOPAW_E_INVALID;
        }
        self.emitted_total.fetch_add(1, Ordering::Relaxed);

        let mut last = ffi::KOPAW_OK;
        for (i, t) in targets.iter().enumerate() {
            if i > 0 {
                unsafe { retain_frame(frame) };
            }
            let rc = match t {
                Target::Queue(q) => match q.send(frame) {
                    SendResult::Ok => ffi::KOPAW_OK,
                    SendResult::Stopped => ffi::KOPAW_E_STOPPED,
                },
                Target::Exec(e) => self.exec_push(e, frame),
            };
            if rc != ffi::KOPAW_OK {
                // 当前引用已被目标侧归还；后续边不再投递
                last = rc;
                break;
            }
        }
        last
    }

    /// 池模式：向执行体推帧（有界 + 条件变量背压），并唤醒 drain 任务。
    fn exec_push(&self, exec: &Arc<NodeExec>, frame: *mut KopawFrame) -> i32 {
        let mut st = lock(&exec.mx);
        loop {
            if exec.stopped.load(Ordering::Acquire)
                || (self.state.load(Ordering::Acquire) as i32) >= ffi::KOPAW_STATE_STOPPING
            {
                exec.stopped.store(true, Ordering::Release);
                unsafe { release_frame(frame) };
                exec.cv.notify_all();
                return ffi::KOPAW_E_STOPPED;
            }
            if st.len() < exec.cap {
                st.push_back(frame);
                break;
            }
            let (s2, _t) = exec
                .cv
                .wait_timeout(st, Duration::from_millis(5))
                .unwrap_or_else(|p| p.into_inner());
            st = s2;
        }
        drop(st);
        // 认领并提交 drain 任务
        if !exec.has_worker.swap(true, Ordering::AcqRel) {
            let sched = lock(&self.sched).clone();
            if let Some(s) = sched {
            let this = GraphRef(self);
            let e = exec.clone();
            s.submit(Box::new(move || this.drain_job(&e)));
            } else {
                // 无调度器（异常状态）：归还帧避免卡死
                exec.has_worker.store(false, Ordering::Release);
                unsafe { release_frame(frame) };
                return ffi::KOPAW_E_STOPPED;
            }
        }
        ffi::KOPAW_OK
    }

    /// 自驱动节点按端口拉取：从指定输入端口队列取一帧，所有权转移给调用方。
    pub fn recv_port(
        &self,
        node: u32,
        port: u32,
        out_frame: &mut *mut KopawFrame,
        timeout_ms: u32,
    ) -> i32 {
        let queue = {
            let g = lock(&self.inner);
            let n = match g.nodes.get(&node) {
                Some(n) => n,
                None => return ffi::KOPAW_E_INVALID,
            };
            match n.inputs.get(port as usize).copied().flatten() {
                Some(lid) => g.links[&lid].queue.clone(),
                None => return ffi::KOPAW_E_INVALID,
            }
        };
        let timeout = if timeout_ms == 0 {
            Duration::ZERO
        } else {
            Duration::from_millis(timeout_ms as u64)
        };
        match queue.recv(timeout) {
            RecvResult::Frame(f) => {
                if let Some(st) = self.stat_of(node) {
                    st.delivered.fetch_add(1, Ordering::Relaxed);
                }
                *out_frame = f; // 所有权转移给调用方（含 EOS 帧）
                ffi::KOPAW_OK
            }
            RecvResult::Timeout => ffi::KOPAW_E_TIMEOUT,
            RecvResult::Stopped => ffi::KOPAW_E_STOPPED,
        }
    }

    /// 兼容入口：单入边节点 = 端口 0。
    pub fn recv(&self, node: u32, out_frame: &mut *mut KopawFrame, timeout_ms: u32) -> i32 {
        self.recv_port(node, 0, out_frame, timeout_ms)
    }

    /// 池模式 drain 任务：串行消费执行体待处理帧并调用节点 send。
    /// （由调度器工作者执行；has_worker 认领保证单节点严格串行。）
    fn drain_job(&self, exec: &Arc<NodeExec>) {
        loop {
            let batch: Vec<*mut KopawFrame> = {
                let mut st = lock(&exec.mx);
                st.drain(..).collect()
            };
            for f in batch {
                if (self.state.load(Ordering::Acquire) as i32) >= ffi::KOPAW_STATE_STOPPING {
                    unsafe { release_frame(f) };
                    continue;
                }
                let had_eos = unsafe { (*f).flags & ffi::KOPAW_FRAME_FLAG_EOS != 0 };
                let t0 = Instant::now();
                let rc = match exec.vt.send {
                    Some(send) => unsafe { send(exec.user, f) },
                    None => ffi::KOPAW_E_GENERIC,
                };
                let us = t0.elapsed().as_micros() as u64;
                exec.stat.delivered.fetch_add(1, Ordering::Relaxed);
                exec.stat.busy_us.fetch_add(us, Ordering::Relaxed);
                exec.stat.busy_max_us.fetch_max(us, Ordering::Relaxed);
                if rc != ffi::KOPAW_OK {
                    // 节点未接管帧；出错后清空残余并停用该执行体
                    unsafe { release_frame(f) };
                    unsafe { self.report_error(&exec.name, rc) };
                    let mut st = lock(&exec.mx);
                    for r in st.drain(..) {
                        unsafe { release_frame(r) };
                    }
                    exec.stopped.store(true, Ordering::Release);
                    exec.cv.notify_all();
                    return;
                }
            }
            // 队列已空：释放认领；锁内复查避免丢失唤醒
            let mut st = lock(&exec.mx);
            if st.is_empty() {
                exec.has_worker.store(false, Ordering::Release);
                if st.is_empty() {
                    return;
                }
                if exec.has_worker.swap(true, Ordering::AcqRel) {
                    return; // 已被其他任务认领
                }
            }
        }
    }

    fn stat_of(&self, node: u32) -> Option<Arc<NodeStat>> {
        let g = lock(&self.inner);
        g.nodes.get(&node).map(|n| n.stat.clone())
    }

    /// 汇聚节点完成记账（延迟记账协议；可从任一节点线程/PA 回调线程调用）。
    /// 全部汇聚节点完成 → 图自然结束（FINISHED，仅触发一次）。
    pub(crate) fn note_sink_input_done(&self) {
        let finished = {
            let mut g = lock(&self.inner);
            if g.sink_done < g.sink_total {
                g.sink_done += 1;
            }
            g.sink_done == g.sink_total && g.sink_total > 0
        };
        if finished {
            let cur = self.state.load(Ordering::Acquire);
            if (cur as i32) == ffi::KOPAW_STATE_RUNNING {
                self.state.store(ffi::KOPAW_STATE_FINISHED as u8, Ordering::Release);
                self.fire_event(ffi::KOPAW_EVENT_FINISHED, ffi::KOPAW_OK, b"all sinks done\0");
            }
            // FINISHED(3) >= STOPPING(2)：所有阻塞发送/接收随之解除
        }
    }

    /// kopaw_node_sink_done：带节点校验的记账入口
    pub fn sink_done(&self, node: u32) -> i32 {
        let is_sink = {
            let g = lock(&self.inner);
            match g.nodes.get(&node) {
                Some(n) => n.is_sink,
                None => return ffi::KOPAW_E_INVALID,
            }
        };
        if !is_sink {
            return ffi::KOPAW_E_INVALID;
        }
        self.note_sink_input_done();
        ffi::KOPAW_OK
    }

    /// 性能基线：JSON 摘要（节点统计 + 链路占用 + 图级计数）
    pub fn stats_json(&self) -> String {
        let g = lock(&self.inner);
        let mut s = String::with_capacity(1024);
        s.push_str("{\"nodes\":[");
        let mut nodes: Vec<_> = g.nodes.iter().collect();
        nodes.sort_by_key(|(id, _)| **id);
        let mut first = true;
        for (id, n) in &nodes {
            if !first {
                s.push(',');
            }
            first = false;
            let st = &n.stat;
            let esc = n.name.replace('\\', "\\\\").replace('"', "\\\"");
            use std::fmt::Write as _;
            let _ = write!(
                s,
                "{{\"id\":{},\"name\":\"{}\",\"delivered\":{},\"busy_us\":{},\"busy_max_us\":{}}}",
                id,
                esc,
                st.delivered.load(Ordering::Relaxed),
                st.busy_us.load(Ordering::Relaxed),
                st.busy_max_us.load(Ordering::Relaxed),
            );
        }
        s.push_str("],\"links\":[");
        let mut links: Vec<_> = g.links.iter().collect();
        links.sort_by_key(|(id, _)| **id);
        let mut first = true;
        for (lid, l) in &links {
            if !first {
                s.push(',');
            }
            first = false;
            let (from, to) = (
                g.nodes.get(&l.src_node).map(|n| n.name.as_str()).unwrap_or("?"),
                g.nodes.get(&l.dst_node).map(|n| n.name.as_str()).unwrap_or("?"),
            );
            use std::fmt::Write as _;
            let _ = write!(
                s,
                "{{\"id\":{},\"from\":\"{}.{}\",\"to\":\"{}\",\"enqueued\":{},\"dequeued\":{},\"max_occ\":{},\"cap\":{}}}",
                lid,
                from,
                l.src_port,
                to,
                l.queue.stats.enqueued.load(Ordering::Relaxed),
                l.queue.stats.dequeued.load(Ordering::Relaxed),
                l.queue.stats.max_occ.load(Ordering::Relaxed),
                l.queue.capacity(),
            );
        }
        use std::fmt::Write as _;
        let _ = write!(
            s,
            "],\"emitted\":{},\"dropped\":{},\"state\":\"{}\"}}",
            self.emitted_total.load(Ordering::Relaxed),
            self.dropped_unconnected.load(Ordering::Relaxed),
            self.state(),
        );
        s
    }

    fn fire_event(&self, ev: KopawEventType, code: i32, msg: &'static [u8]) {
        let e = lock(&self.event);
        if let Some(cb) = e.cb {
            unsafe { cb(e.user, ev, code, msg.as_ptr() as *const _) };
        }
    }

    unsafe fn report_error(&self, _node: &str, code: i32) {
        let cur = self.state.load(Ordering::Acquire);
        if (cur as i32) == ffi::KOPAW_STATE_RUNNING {
            self.state.store(ffi::KOPAW_STATE_ERROR as u8, Ordering::Release);
            self.fire_event(ffi::KOPAW_EVENT_ERROR, code, b"node error\0");
        }
        // 诊断细节留给日志系统（MVP 不打印）
    }
}

// ---------------------------------------------------------------------------
// 内部辅助类型
// ---------------------------------------------------------------------------

enum NodeKind {
    Source,
    SelfDriven,
    Reactive,
}

struct NodeJob {
    node: u32,
    user: *mut c_void,
    vt: KopawNodeVTable,
    is_sink: bool,
    self_driven: bool,
    name: String,
    stat: Arc<NodeStat>,
    in_queue: Option<Arc<FrameQueue>>,
}

// 跨线程移交 NodeJob 是安全的：user 指向的 C++ 节点对象按 ABI 契约
// 在 graph_free（join 完成后）前保持有效且可从任意线程触碰；
// 其余字段为拷贝语义的函数指针、布尔量、Arc。
unsafe impl Send for NodeJob {}

struct GraphRef(*const GraphCore);
unsafe impl Send for GraphRef {}
unsafe impl Sync for GraphRef {}

impl GraphRef {
    unsafe fn note_sink_input_done(&self) {
        (*self.0).note_sink_input_done();
    }
    unsafe fn report_error(&self, name: &str, code: i32) {
        (*self.0).report_error(name, code);
    }
    fn drain_job(&self, e: &Arc<NodeExec>) {
        unsafe { (*self.0).drain_job(e) }
    }
}

// ---------------------------------------------------------------------------
// 单元测试：用 Rust 模拟 C++ 节点，验证所有权/停止/EOS/自然结束语义
// ---------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::{
        KOPAW_E_STOPPED, KOPAW_E_TIMEOUT, KOPAW_EVENT_ERROR, KOPAW_EVENT_FINISHED,
        KOPAW_FRAME_FLAG_EOS, KOPAW_MEDIA_VIDEO, KOPAW_OK, KOPAW_STATE_FINISHED,
        KOPAW_STATE_STOPPING,
    };
    use std::sync::atomic::AtomicU32;
    use std::sync::atomic::AtomicUsize;
    use std::mem::size_of;

    /// 每测试独立的泄漏计数与引用计数头：帧的 dma_buf_handle 指向 MockHdr
    /// （mock 帧不承载媒体数据，引擎永不解码该句柄，借用它是安全的）。
    struct MockHdr {
        refs: AtomicU32,
        ctr: *const AtomicUsize,
    }

    unsafe extern "C" fn mock_retain(f: *mut KopawFrame) {
        let h = (*f).dma_buf_handle as usize as *const MockHdr;
        (*h).refs.fetch_add(1, Ordering::SeqCst);
    }

    unsafe extern "C" fn mock_release(f: *mut KopawFrame) {
        let h = (*f).dma_buf_handle as usize as *const MockHdr;
        if (*h).refs.fetch_sub(1, Ordering::SeqCst) == 1 {
            (*(*h).ctr).fetch_sub(1, Ordering::SeqCst);
            drop(Box::from_raw(h as *mut MockHdr));
            drop(Box::from_raw(f));
        }
    }

    unsafe fn make_frame(ctr: &AtomicUsize, pts: i64, eos: bool) -> *mut KopawFrame {
        ctr.fetch_add(1, Ordering::SeqCst); // 计入一个“块”
        let hdr = Box::into_raw(Box::new(MockHdr {
            refs: AtomicU32::new(1),
            ctr: ctr as *const AtomicUsize,
        }));
        Box::into_raw(Box::new(KopawFrame {
            struct_size: std::mem::size_of::<KopawFrame>() as u32,
            media_type: KOPAW_MEDIA_VIDEO,
            flags: if eos { KOPAW_FRAME_FLAG_EOS } else { 0 },
            pts,
            dts: pts,
            format: std::mem::zeroed(),
            dma_buf_handle: hdr as usize as u64,
            size: 0,
            stride: 0,
            memory_type: 0,
            dma_fd: -1,
            plane_count: 0,
            planes: [std::mem::zeroed(); 4],
            acquire_fence: crate::ffi::KopawSyncFence {
                kind: crate::ffi::KOPAW_SYNC_FENCE_NONE,
                fd: -1,
                value: 0,
            },
            user_data: std::ptr::null_mut(),
            retain: Some(mock_retain),
            release: Some(mock_release),
        }))
    }

    struct SinkState {
        g: *const GraphCore,
        got: AtomicUsize,
        finished: AtomicUsize,
        errors: AtomicUsize,
    }

    unsafe extern "C" fn sink_send(user: *mut c_void, f: *mut KopawFrame) -> i32 {
        let st = &*(user as *const SinkState);
        st.got.fetch_add(1, Ordering::SeqCst);
        let eos = (*f).flags & KOPAW_FRAME_FLAG_EOS != 0;
        release_frame(f);
        if eos {
            // P1 延迟记账协议：汇聚节点自行记账
            (*st.g).note_sink_input_done();
        }
        KOPAW_OK
    }

    /// 慢速汇聚：每帧 5ms，制造积压以测试运行中停止
    unsafe extern "C" fn slow_sink_send(user: *mut c_void, f: *mut KopawFrame) -> i32 {
        let st = &*(user as *const SinkState);
        std::thread::sleep(Duration::from_millis(5));
        st.got.fetch_add(1, Ordering::SeqCst);
        release_frame(f);
        KOPAW_OK
    }

    unsafe extern "C" fn sink_destroy(user: *mut c_void) {
        let _ = Box::from_raw(user as *mut SinkState);
    }

    /// 透传节点的宿主状态
    struct PassState {
        out: KopawOutput,
        g: *const GraphCore,
    }

    unsafe extern "C" fn pass_destroy(user: *mut c_void) {
        let _ = Box::from_raw(user as *mut PassState);
    }

    unsafe extern "C" fn event_cb(user: *mut c_void, ev: KopawEventType, _code: i32, _m: *const i8) {
        let st = &*(user as *const SinkState);
        match ev {
            KOPAW_EVENT_FINISHED => st.finished.fetch_add(1, Ordering::SeqCst),
            KOPAW_EVENT_ERROR => st.errors.fetch_add(1, Ordering::SeqCst),
            _ => 0,
        };
    }

    fn add_named_node(
        g: &GraphCore,
        name: &str,
        user: *mut c_void,
        vt: KopawNodeVTable,
        outputs: u32,
        is_sink: bool,
        self_driven: bool,
    ) -> u32 {
        g.add_node(name.to_string(), user, &vt, outputs, 1, 4, is_sink, self_driven)
            .expect("add_node failed")
    }

    /// 源节点：发 n 帧数据 + 1 帧 EOS 后返回
    struct SrcState {
        out: KopawOutput,
        g: *const GraphCore,
        leak: *const AtomicUsize,
        n: i64,
    }

    unsafe extern "C" fn source_run(user: *mut c_void) -> i32 {
        let s = &*(user as *const SrcState);
        for i in 0..s.n {
            let f = make_frame(&*s.leak, i, false);
            if (*s.g).emit(s.out, f) != KOPAW_OK {
                return KOPAW_OK; // 停止时 emit 已归还当前引用
            }
        }
        let eos = make_frame(&*s.leak, s.n, true);
        (*s.g).emit(s.out, eos);
        KOPAW_OK
    }

    /// 透传节点：把收到的帧原样 emit 下游（emit 接管当前引用，不再 release）
    unsafe extern "C" fn pass_send(user: *mut c_void, f: *mut KopawFrame) -> i32 {
        let s = &*(user as *const PassState);
        (*s.g).emit(s.out, f)
    }

    #[test]
    fn source_pass_sink_natural_finish_no_leak() {
        unsafe {
        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 },
            g: &g as *const _,
            leak: &leak,
            n: 10,
        }));
        let pass = Box::into_raw(Box::new(PassState { out: KopawOutput { id: 0 }, g: &g as *const _ }));
        let sink = Box::into_raw(Box::new(SinkState {
            g: &g as *const _,
            got: AtomicUsize::new(0),
            finished: AtomicUsize::new(0),
            errors: AtomicUsize::new(0),
        }));

        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let pass_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(pass_send),
            stop: None,
            destroy: Some(pass_destroy),
            bind_output: None,
        };
        let sink_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(sink_send),
            stop: None,
            destroy: Some(sink_destroy),
            bind_output: None,
        };

        let n_src = add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        let n_pass = add_named_node(&g, "pass", pass as _, pass_vt, 1, false, false);
        let n_sink = add_named_node(&g, "sink", sink as _, sink_vt, 0, true, false);
        // 注册后回填真实输出句柄
        (*src).out = g.node_output(n_src, 0).unwrap();
        (*pass).out = g.node_output(n_pass, 0).unwrap();

        let o_src = g.node_output(n_src, 0).unwrap();
        let o_pass = g.node_output(n_pass, 0).unwrap();
        assert_eq!(g.connect(o_src, n_pass, 0, 4), KOPAW_OK);
        assert_eq!(g.connect(o_pass, n_sink, 0, 4), KOPAW_OK);

        g.set_event_cb(Some(event_cb), sink as *mut c_void);
        assert_eq!(g.start(), KOPAW_OK);

        // 自然结束：等待 FINISHED（源发完即走，透传即时）
        for _ in 0..500 {
            if g.state() == KOPAW_STATE_FINISHED {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(g.state(), KOPAW_STATE_FINISHED);
        assert_eq!((*sink).got.load(Ordering::SeqCst), 11); // 10 数据 + 1 EOS
        assert_eq!((*sink).finished.load(Ordering::SeqCst), 1);

        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0, "frames leaked");
        // shutdown 已调用 destroy；避免双重释放
        let _ = Box::from_raw(src);
        }
    }

    #[test]
    fn stop_mid_stream_no_leak() {
        unsafe {
        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        // 慢速汇聚：每帧 5ms，制造队列积压与阻塞发送
        let sink = Box::into_raw(Box::new(SinkState {
            g: &g as *const _,
            got: AtomicUsize::new(0),
            finished: AtomicUsize::new(0),
            errors: AtomicUsize::new(0),
        }));
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 },
            g: &g as *const _,
            leak: &leak,
            n: 1_000_000,
        }));

        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let sink_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(slow_sink_send),
            stop: None,
            destroy: Some(sink_destroy),
            bind_output: None,
        };
        let n_src = add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        let n_sink = add_named_node(&g, "sink", sink as _, sink_vt, 0, true, false);
        (*src).out = g.node_output(n_src, 0).unwrap();
        let o = g.node_output(n_src, 0).unwrap();
        assert_eq!(g.connect(o, n_sink, 0, 2), KOPAW_OK);
        assert_eq!(g.start(), KOPAW_OK);

        std::thread::sleep(Duration::from_millis(50));
        assert_eq!(g.stop(), KOPAW_OK);
        assert_eq!(g.state(), KOPAW_STATE_STOPPING);
        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0, "frames leaked on stop");
        let _ = Box::from_raw(src);
        }
    }

    #[test]
    fn self_driven_sink_recv_eos() {
        unsafe {
        static GOT: AtomicUsize = AtomicUsize::new(0);

        unsafe extern "C" fn driven_run(user: *mut c_void) -> i32 {
            let st = &*(user as *const DrvState);
            loop {
                let mut f: *mut KopawFrame = std::ptr::null_mut();
                match (*st.g).recv(st.node, &mut f, 10) {
                    KOPAW_OK => {
                        GOT.fetch_add(1, Ordering::SeqCst);
                        let eos = (*f).flags & KOPAW_FRAME_FLAG_EOS != 0;
                        release_frame(f);
                        if eos {
                            // 延迟记账：自驱动汇聚自行记账
                            (*st.g).note_sink_input_done();
                            return KOPAW_OK;
                        }
                    }
                    KOPAW_E_TIMEOUT => continue,
                    KOPAW_E_STOPPED => return KOPAW_OK,
                    _ => return 9,
                }
            }
        }

        struct DrvState {
            g: *const GraphCore,
            node: u32,
        }

        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 },
            g: &g as *const _,
            leak: &leak,
            n: 5,
        }));
        let drv = Box::into_raw(Box::new(DrvState { g: &g as *const _, node: 0 }));

        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let drv_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(driven_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let n_src = add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        let n_drv = add_named_node(&g, "drv", drv as _, drv_vt, 0, true, true);
        // 注册后回填自身节点 id，供 run 内的 recv 使用
        (*drv).node = n_drv;
        (*src).out = g.node_output(n_src, 0).unwrap();
        let o = g.node_output(n_src, 0).unwrap();
        assert_eq!(g.connect(o, n_drv, 0, 4), KOPAW_OK);
        assert_eq!(g.start(), KOPAW_OK);

        for _ in 0..1000 {
            if g.state() == KOPAW_STATE_FINISHED {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(g.state(), KOPAW_STATE_FINISHED);
        assert_eq!(GOT.load(Ordering::SeqCst), 6);
        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0);
        let _ = Box::from_raw(src);
        let _ = Box::from_raw(drv);
        }
    }

    #[test]
    fn unconnected_output_drops_frames() {
        unsafe {
        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 }, // 未连接
            g: &g as *const _,
            leak: &leak,
            n: 3,
        }));
        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        // 无汇聚节点：不会自然结束，靠 run 自己返回
        add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        assert_eq!(g.start(), KOPAW_OK);
        std::thread::sleep(Duration::from_millis(30));
        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0, "unconnected frames must be released");
        assert_eq!(g.dropped_unconnected.load(Ordering::SeqCst), 4);
        let _ = Box::from_raw(src);
        }
    }

    /// P1 tee 广播：同一输出连两个汇聚节点，帧经 retain 共享，
    /// 两个汇聚都记账后才 FINISHED，引用计数归零无泄漏。
    #[test]
    fn tee_broadcast_two_sinks_no_leak() {
        unsafe {
        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 },
            g: &g as *const _,
            leak: &leak,
            n: 8,
        }));
        let mk_sink = |g: *const GraphCore| {
            Box::into_raw(Box::new(SinkState {
                g,
                got: AtomicUsize::new(0),
                finished: AtomicUsize::new(0),
                errors: AtomicUsize::new(0),
            }))
        };
        let sink0 = mk_sink(&g as *const _);
        let sink1 = mk_sink(&g as *const _);

        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let sink_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(sink_send),
            stop: None,
            destroy: Some(sink_destroy),
            bind_output: None,
        };
        let n_src = add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        let n_s0 = add_named_node(&g, "sink0", sink0 as _, sink_vt, 0, true, false);
        let n_s1 = add_named_node(&g, "sink1", sink1 as _, sink_vt, 0, true, false);
        (*src).out = g.node_output(n_src, 0).unwrap();

        let o = g.node_output(n_src, 0).unwrap();
        // 同一输出端口连两条边（tee）
        assert_eq!(g.connect(o, n_s0, 0, 4), KOPAW_OK);
        assert_eq!(g.connect(o, n_s1, 0, 4), KOPAW_OK);

        g.set_event_cb(Some(event_cb), sink0 as *mut c_void);
        assert_eq!(g.start(), KOPAW_OK);

        for _ in 0..1000 {
            if g.state() == KOPAW_STATE_FINISHED {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(g.state(), KOPAW_STATE_FINISHED, "两个汇聚都记账后才 FINISHED");
        assert_eq!((*sink0).got.load(Ordering::SeqCst), 9); // 8 数据 + 1 EOS
        assert_eq!((*sink1).got.load(Ordering::SeqCst), 9);
        assert_eq!((*sink0).finished.load(Ordering::SeqCst), 1);

        // 统计 JSON 应包含两个汇聚与统计字段
        let j = g.stats_json();
        assert!(j.contains("\"name\":\"src\"") && j.contains("\"name\":\"sink0\""));
        assert!(j.contains("\"emitted\":9"));

        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0, "tee 引用计数必须归零");
        let _ = Box::from_raw(src);
        }
    }

    /// P2 池调度：与 source_pass_sink 相同的图，用 worker 池代替每节点一线程。
    /// 验证串行化 drain 的顺序性、FINISHED 与零泄漏。
    #[test]
    fn pool_scheduler_source_pass_sink_no_leak() {
        unsafe {
        let leak = AtomicUsize::new(0);
        let g = GraphCore::new();
        assert_eq!(g.set_workers(4), 4);
        let src = Box::into_raw(Box::new(SrcState {
            out: KopawOutput { id: 0 },
            g: &g as *const _,
            leak: &leak,
            n: 50,
        }));
        let pass = Box::into_raw(Box::new(PassState { out: KopawOutput { id: 0 }, g: &g as *const _ }));
        let sink = Box::into_raw(Box::new(SinkState {
            g: &g as *const _,
            got: AtomicUsize::new(0),
            finished: AtomicUsize::new(0),
            errors: AtomicUsize::new(0),
        }));
        let src_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: Some(source_run),
            send: None,
            stop: None,
            destroy: None,
            bind_output: None,
        };
        let pass_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(pass_send),
            stop: None,
            destroy: Some(pass_destroy),
            bind_output: None,
        };
        let sink_vt = KopawNodeVTable {
            struct_size: size_of::<KopawNodeVTable>() as u32,
            run: None,
            send: Some(sink_send),
            stop: None,
            destroy: Some(sink_destroy),
            bind_output: None,
        };
        let n_src = add_named_node(&g, "src", src as _, src_vt, 1, false, false);
        let n_pass = add_named_node(&g, "pass", pass as _, pass_vt, 1, false, false);
        let n_sink = add_named_node(&g, "sink", sink as _, sink_vt, 0, true, false);
        (*src).out = g.node_output(n_src, 0).unwrap();
        (*pass).out = g.node_output(n_pass, 0).unwrap();
        let o_src = g.node_output(n_src, 0).unwrap();
        let o_pass = g.node_output(n_pass, 0).unwrap();
        assert_eq!(g.connect(o_src, n_pass, 0, 4), KOPAW_OK);
        assert_eq!(g.connect(o_pass, n_sink, 0, 4), KOPAW_OK);
        g.set_event_cb(Some(event_cb), sink as *mut c_void);
        assert_eq!(g.start(), KOPAW_OK);

        for _ in 0..1000 {
            if g.state() == KOPAW_STATE_FINISHED {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(g.state(), KOPAW_STATE_FINISHED);
        assert_eq!((*sink).got.load(Ordering::SeqCst), 51);
        g.shutdown();
        assert_eq!(leak.load(Ordering::SeqCst), 0, "pool mode frames leaked");
        let _ = Box::from_raw(src);
        }
    }
}
