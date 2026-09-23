//! C ABI 类型定义与导出函数。
//!
//! 本模块是 Rust 引擎与 C++ 节点实现之间的唯一边界：
//! - 所有类型 `#[repr(C)]`，头文件 `kopaw/abi/include/kopaw_abi.h` 由 cbindgen 生成；
//! - C++ 侧只依赖生成的头文件，不 include 任何 Rust 内容。

use std::ffi::CStr;
use std::mem::size_of;
use std::os::raw::{c_char, c_void};

// ---------------------------------------------------------------------------
// Stable ABI identity and feature negotiation
// ---------------------------------------------------------------------------

// A major bump is required when the layout or ownership contract changes.
// Minor versions are backwards-compatible additions and are negotiated by
// the plugin loader before any node descriptor is used.
pub const KOPAW_ABI_MAJOR: u32 = 5;
/// 5.1: KOPAW_MEMORY_VULKAN external-memory frames (P3-M3). The frame layout
/// was unchanged; only the new memory-type value and capability bit were added.
/// 5.2: P2 platformization — KopawNodeVTable::send_port for reactive
/// multi-input nodes (KOPAW_CAP_MULTI_INPUT) and KopawFrame::drm_fourcc for
/// external-memory frames. Both are additive: newer fields are appended and
/// gated by struct_size, so 5.0/5.1 nodes and frames keep working.
/// 5.3: explicit frame colorimetry and optional HDR metadata. The fields are
/// appended to KopawFrame and negotiated with KOPAW_CAP_COLOR_METADATA.
pub const KOPAW_ABI_MINOR: u32 = 3;
pub const KOPAW_ABI_VERSION: u32 = 0x0005_0003;

pub const KOPAW_CAP_FRAME_OWNERSHIP: u64 = 0x0001;
pub const KOPAW_CAP_NODE_LIFECYCLE: u64 = 0x0002;
pub const KOPAW_CAP_MULTI_OUTPUT: u64 = 0x0004;
pub const KOPAW_CAP_SELF_DRIVEN: u64 = 0x0008;
pub const KOPAW_CAP_DMABUF_FRAMES: u64 = 0x0010;
pub const KOPAW_CAP_POOL_SCHEDULER: u64 = 0x0020;
pub const KOPAW_CAP_NODE_OUTPUT_BINDING: u64 = 0x0040;
/// Media memory is addressed by the opaque dma_buf_handle field.  CPU-mode
/// consumers may decode it as a local address; it must never cross a process.
pub const KOPAW_CAP_HANDLE_FRAMES: u64 = 0x0080;
/// Frames whose memory is a Vulkan device allocation with exportable external
/// memory. In-process consumers use the producer's VkImage; cross-process
/// consumers import the exported DMA-BUF plane described in planes[].
pub const KOPAW_CAP_VULKAN_EXTERNAL: u64 = 0x0100;
/// Reactive nodes may declare multiple input ports when their vtable provides
/// the send_port callback (5.2). Self-driven multi-input nodes (recv_port)
/// do not require this bit.
pub const KOPAW_CAP_MULTI_INPUT: u64 = 0x0200;
/// Frames carry explicit color range/matrix/transfer and optional HDR data.
pub const KOPAW_CAP_COLOR_METADATA: u64 = 0x0400;
pub const KOPAW_ABI_CAPABILITIES: u64 = 0x07ff;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawAbiInfo {
    pub struct_size: u32,
    pub major: u32,
    pub minor: u32,
    /// Capabilities required by a plugin, or provided by the host ABI.
    pub capabilities: u64,
}

// ---------------------------------------------------------------------------
// 媒体类型与帧格式（MVP 约定：视频 = RGBA8 打包，音频 = f32 交错）
// ---------------------------------------------------------------------------

pub type KopawMediaType = i32;

pub const KOPAW_MEDIA_VIDEO: KopawMediaType = 0;
pub const KOPAW_MEDIA_AUDIO: KopawMediaType = 1;

/// 帧标志位
pub const KOPAW_FRAME_FLAG_EOS: u32 = 1 << 0;
/// 解码输入帧：关键帧（demuxer 从 AV_PKT_FLAG_KEY 映射）
pub const KOPAW_FRAME_FLAG_KEY: u32 = 1 << 1;

/// 帧内存类型
pub const KOPAW_MEMORY_CPU: u32 = 0;
/// DMA-BUF 外部内存：dma_buf_handle 和 DMA-BUF 平面有效；size 由格式决定，
/// 最后一个 release 负责关闭 fd。
pub const KOPAW_MEMORY_DMABUF: u32 = 1;
/// Vulkan external memory：内存由生产者的 Vulkan 设备持有并带 DMA-BUF 导出。
/// dma_buf_handle 是生产者本地的 VkImage 句柄（仅同进程消费有效），跨进程
/// 消费必须导入 planes[] 中的 DMA-BUF fd；fd 同样由最后一个 release 关闭。
pub const KOPAW_MEMORY_VULKAN: u32 = 2;

/// DMA-BUF 平面上限；多平面格式可用 planes[] 描述。
pub const KOPAW_MAX_DMABUF_PLANES: u32 = 4;

/// 同步栅栏类型。
pub const KOPAW_SYNC_FENCE_NONE: u32 = 0;
pub const KOPAW_SYNC_FENCE_FD: u32 = 1;
pub const KOPAW_SYNC_FENCE_TIMELINE: u32 = 2;

/// 返回码：send / run / emit / recv / connect / start / stop 共用
pub const KOPAW_OK: i32 = 0;
pub const KOPAW_E_STOPPED: i32 = 1; // 图已停止，操作未生效
pub const KOPAW_E_TIMEOUT: i32 = 2; // recv 超时
pub const KOPAW_E_EOS: i32 = 3; // 保留：输入通道已终结（MVP 用 EOS 帧传递）
pub const KOPAW_E_INVALID: i32 = 4; // 参数/状态非法
pub const KOPAW_E_GENERIC: i32 = 5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawVideoFormat {
    pub width: u32,
    pub height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawAudioFormat {
    pub sample_rate: u32,
    pub channels: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union KopawFormat {
    pub video: KopawVideoFormat,
    pub audio: KopawAudioFormat,
}

/// DMA-BUF 平面元数据。fd、offset、stride 和 modifier 的所有权由帧的
/// release 回调管理；消费者不得自行关闭 fd。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawDmabufPlane {
    pub fd: i32,
    pub offset: u32,
    pub stride: u32,
    pub modifier: u64,
}

/// 帧进入合成器前需要等待的同步对象。FD 的生命周期同样由 release 管理。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawSyncFence {
    pub kind: u32,
    pub fd: i32,
    pub value: u64,
}

// ---------------------------------------------------------------------------
// Frame colorimetry (5.3, additive tail)
// ---------------------------------------------------------------------------

pub const KOPAW_COLOR_RANGE_UNKNOWN: u32 = 0;
pub const KOPAW_COLOR_RANGE_LIMITED: u32 = 1;
pub const KOPAW_COLOR_RANGE_FULL: u32 = 2;

pub const KOPAW_COLOR_MATRIX_UNKNOWN: u32 = 0;
pub const KOPAW_COLOR_MATRIX_BT601: u32 = 1;
pub const KOPAW_COLOR_MATRIX_BT709: u32 = 2;
pub const KOPAW_COLOR_MATRIX_BT2020_NCL: u32 = 3;
pub const KOPAW_COLOR_MATRIX_BT2020_CL: u32 = 4;

pub const KOPAW_COLOR_TRANSFER_UNKNOWN: u32 = 0;
pub const KOPAW_COLOR_TRANSFER_BT709: u32 = 1;
pub const KOPAW_COLOR_TRANSFER_SRGB: u32 = 2;
pub const KOPAW_COLOR_TRANSFER_GAMMA22: u32 = 3;
pub const KOPAW_COLOR_TRANSFER_PQ: u32 = 4;
pub const KOPAW_COLOR_TRANSFER_HLG: u32 = 5;

pub const KOPAW_COLOR_PRIMARIES_UNKNOWN: u32 = 0;
pub const KOPAW_COLOR_PRIMARIES_BT601: u32 = 1;
pub const KOPAW_COLOR_PRIMARIES_BT709: u32 = 2;
pub const KOPAW_COLOR_PRIMARIES_BT2020: u32 = 3;
pub const KOPAW_COLOR_PRIMARIES_P3: u32 = 4;

pub const KOPAW_CHROMA_LOCATION_UNKNOWN: u32 = 0;
pub const KOPAW_CHROMA_LOCATION_LEFT: u32 = 1;
pub const KOPAW_CHROMA_LOCATION_CENTER: u32 = 2;
pub const KOPAW_CHROMA_LOCATION_TOPLEFT: u32 = 3;
pub const KOPAW_CHROMA_LOCATION_TOP: u32 = 4;
pub const KOPAW_CHROMA_LOCATION_BOTTOMLEFT: u32 = 5;
pub const KOPAW_CHROMA_LOCATION_BOTTOM: u32 = 6;

pub const KOPAW_HDR_FLAG_MASTERING_DISPLAY: u32 = 1 << 0;
pub const KOPAW_HDR_FLAG_CONTENT_LIGHT: u32 = 1 << 1;

/// Optional HDR side data. Chromaticity coordinates use a 100000 scale;
/// luminance is milli-cd/m^2 and MaxCLL/MaxFALL are cd/m^2.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KopawHdrMetadata {
    pub flags: u32,
    pub max_luminance: u32,
    pub min_luminance: u32,
    pub max_cll: u32,
    pub max_fall: u32,
    pub display_primaries: [u32; 6],
    pub white_point: [u32; 2],
    pub reserved: [u32; 1],
}

/// Explicit frame colorimetry. Unknown values are preserved as unknown; a
/// renderer must not infer BT.601/709 from frame dimensions.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KopawColorMetadata {
    pub range: u32,
    pub matrix: u32,
    pub transfer: u32,
    pub primaries: u32,
    pub chroma_location: u32,
    pub flags: u32,
    pub hdr: KopawHdrMetadata,
}

/// 输出端口句柄（emit 的寻址凭据）。
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct KopawOutput {
    pub id: u64,
}

/// 跨 ABI 传递的媒体帧。所有权模型：**引用计数 + 独占释放**。
///
/// - 生产者分配（refs 从 1 开始）；队列/引擎只搬运指针，不解引用数据；
/// - 单消费者（独占转移）场景：消费完调用 `release(frame)`；
/// - 多消费者（tee/多出边）：引擎在向后续链路投递前调用 `retain(frame)`，
///   每个消费者各自 release，最后一个引用真正释放。**retain 必须非空**
///   （引擎检测到多链路且 retain 缺失时会丢弃帧并返回 E_INVALID）；
/// - 引擎在停止与析构时对队列中滞留的帧统一调用 release，不丢不漏；
/// - `dma_buf_handle` 是唯一的媒体内存标识：KOPAW_MEMORY_CPU 下它是当前
///   地址空间内的地址别名，KOPAW_MEMORY_DMABUF 下它是本地 DMA-BUF 句柄，
///   KOPAW_MEMORY_VULKAN 下它是生产者本地的 VkImage 句柄；
///   它不能作为跨进程的数字 FD 使用，跨进程传输必须走拥有 FD 的协议层。
#[repr(C)]
pub struct KopawFrame {
    /// 必须设置为 sizeof(KopawFrame) 或不小于宿主所需的前缀大小。
    pub struct_size: u32,
    pub media_type: KopawMediaType,
    pub flags: u32,
    /// 微秒级媒体时间戳（展示时间）
    pub pts: i64,
    /// 微秒级解码时间戳（B 帧重排；无意义时等于 pts）
    pub dts: i64,
    pub format: KopawFormat,
    /// 媒体内存句柄。CPU 帧为当前地址空间内的地址别名，DMA-BUF 帧为
    /// 本地句柄；不要把这个整数当作跨进程 FD 传递。
    pub dma_buf_handle: u64,
    /// 数据字节数
    pub size: usize,
    /// 视频每行字节数；音频为 0
    pub stride: u32,
    /// 内存类型：KOPAW_MEMORY_CPU / KOPAW_MEMORY_DMABUF
    pub memory_type: u32,
    /// DMA-BUF 文件描述符（CPU 内存时为 -1）
    pub dma_fd: i32,
    /// DMA-BUF 平面；CPU 帧为 0。单平面帧可同时填 dma_fd 和 planes[0]。
    pub plane_count: u32,
    pub planes: [KopawDmabufPlane; 4],
    pub acquire_fence: KopawSyncFence,
    /// 生产者私有上下文，不由引擎解释。
    pub user_data: *mut c_void,
    /// 增加一个引用（tee/多出边投递时由引擎调用）
    pub retain: Option<unsafe extern "C" fn(frame: *mut KopawFrame)>,
    /// 释放回调：引用归零时释放帧头及所属数据块。由生产者实现，
    /// 引擎与消费者都会调用。
    pub release: Option<unsafe extern "C" fn(frame: *mut KopawFrame)>,
    /// 5.2：外部内存帧的 DRM fourcc（如 NV12 = 0x3231_564E）。CPU 帧为 0，
    /// 像素布局由 media_type 的既有约定解释（视频 = RGBA8 打包）；消费方仅在
    /// 读取外部平面时使用此字段。结构体按 struct_size 前缀兼容读取。
    pub drm_fourcc: u32,
    /// 5.3：显式色彩范围、矩阵、传递函数、原色、色度位置和可选 HDR 元数据。
    /// 消费方必须先检查 struct_size 是否覆盖此字段；未知值不得按高度猜测。
    pub color: KopawColorMetadata,
}

// ---------------------------------------------------------------------------
// 节点描述与虚表
// ---------------------------------------------------------------------------

/// 节点实现侧回调（由 C++ 实现）。
///
/// 所有权约定（send）：
/// - 返回 KOPAW_OK：节点接管帧，之后由节点调用 release；
/// - 返回非 0：节点未接管，引擎立即 release。
///
/// 注意：函数指针类型必须内联书写（不要经 type alias 间接引用），
/// 否则 cbindgen 无法将其解析为可空 C 函数指针。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KopawNodeVTable {
    pub struct_size: u32,
    /// 源节点/自驱动节点的主体循环，返回即视为节点退出
    pub run: Option<unsafe extern "C" fn(user: *mut c_void) -> i32>,
    /// 响应式节点收到一帧（所有权转移见上）
    pub send: Option<unsafe extern "C" fn(user: *mut c_void, frame: *mut KopawFrame) -> i32>,
    /// 停止提示：节点应尽快让阻塞点退出
    pub stop: Option<unsafe extern "C" fn(user: *mut c_void)>,
    /// 图析构时调用，节点释放自身资源（此时刻节点线程必然已全部退出）
    pub destroy: Option<unsafe extern "C" fn(user: *mut c_void)>,
    /// 图注册后绑定输出端口句柄；没有输出端口的节点可省略。
    pub bind_output:
        Option<unsafe extern "C" fn(user: *mut c_void, port: u32, output: KopawOutput)>,
    /// 5.2：多入边响应式节点按端口收帧（所有权转移约定同 send）。
    /// 声明 inputs > 1 且非自驱动的节点必须提供；单入边响应式节点继续使用
    /// send，自驱动节点走 run + kopaw_node_recv(_port)。
    pub send_port:
        Option<unsafe extern "C" fn(user: *mut c_void, frame: *mut KopawFrame, port: u32) -> i32>,
}

/// Minimum prefix required by the engine. Fields added by a minor ABI
/// revision must be guarded by the table's struct_size before they are read.
pub(crate) const KOPAW_NODE_VTABLE_BASE_SIZE: usize =
    std::mem::offset_of!(KopawNodeVTable, destroy)
        + size_of::<Option<unsafe extern "C" fn(user: *mut c_void)>>();

#[repr(C)]
pub struct KopawNodeDesc {
    pub struct_size: u32,
    /// 诊断用名称（引擎会拷贝）
    pub name: *const c_char,
    pub user_data: *mut c_void,
    /// 输出端口数（0 = 纯汇聚节点）
    pub outputs: u32,
    /// 输入端口数：响应式节点单入边（send）或声明多入边（必须提供 send_port）；
    /// 自驱动节点可声明任意多个（run 内按端口 kopaw_node_recv_port 拉取）。
    pub inputs: u32,
    /// 每条入边的有界队列容量
    pub queue_capacity: u32,
    /// 汇聚节点：需自行经 kopaw_node_sink_done 记账；全部汇聚完成 → 图 Finished
    pub is_sink: u8,
    /// 自驱动：引擎不代为 pop，节点在 run() 中用 kopaw_node_recv(_port) 拉取
    pub self_driven: u8,
    pub vtable: *const KopawNodeVTable,
}

/// 图状态
pub const KOPAW_STATE_IDLE: i32 = 0;
pub const KOPAW_STATE_RUNNING: i32 = 1;
pub const KOPAW_STATE_STOPPING: i32 = 2;
pub const KOPAW_STATE_FINISHED: i32 = 3;
pub const KOPAW_STATE_ERROR: i32 = 4;

pub type KopawEventType = i32;
pub const KOPAW_EVENT_FINISHED: KopawEventType = 0;
pub const KOPAW_EVENT_ERROR: KopawEventType = 1;

/// 事件回调：可能在任一引擎线程上触发，C++ 侧需保证线程安全。
/// 同样必须内联书写函数指针类型（cbindgen 限制）。
pub(crate) type KopawEventFn =
    unsafe extern "C" fn(user: *mut c_void, ev: KopawEventType, code: i32, msg: *const c_char);

// ---------------------------------------------------------------------------
// 导出 API
// ---------------------------------------------------------------------------

/// 图句柄。结构体内容对 C++ 不透明。
/// cbindgen:opaque
#[repr(C)]
pub struct KopawGraph {
    _private: [u8; 0],
}

unsafe fn desc_name(desc: &KopawNodeDesc) -> String {
    if desc.name.is_null() {
        "anonymous".to_string()
    } else {
        CStr::from_ptr(desc.name).to_string_lossy().into_owned()
    }
}

// panic 安全约定：FFI 边界不允许 Rust panic 逃逸。
// 内部实现不使用 .unwrap()/expect()；互斥量按“免毒化”方式访问
// （lock() 失败时 into_inner() 继续工作），因此不会因 panic 而死锁或 unwind。

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_new() -> *mut KopawGraph {
    let boxed = Box::new(crate::graph::GraphCore::new());
    Box::into_raw(boxed) as *mut KopawGraph
}

/// 析构：先停止并回收全部线程与滞留帧，再逐节点调用 destroy。
/// 传入空指针安全。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_free(g: *mut KopawGraph) {
    if g.is_null() {
        return;
    }
    let core = Box::from_raw(g as *mut crate::graph::GraphCore);
    core.shutdown();
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_add_node(
    g: *mut KopawGraph,
    desc: *const KopawNodeDesc,
) -> u32 {
    if g.is_null() || desc.is_null() {
        return 0;
    }
    let d = &*desc;
    if d.struct_size < size_of::<KopawNodeDesc>() as u32 || d.vtable.is_null() {
        return 0;
    }
    let source_vt = &*d.vtable;
    if (source_vt.struct_size as usize) < KOPAW_NODE_VTABLE_BASE_SIZE {
        return 0;
    }
    // Copy only the bytes advertised by the plugin. This keeps an older
    // minor-version vtable safe while zero-initializing newer optional fields.
    let mut vt = KopawNodeVTable {
        struct_size: 0,
        run: None,
        send: None,
        stop: None,
        destroy: None,
        bind_output: None,
        send_port: None,
    };
    let copy_size = std::cmp::min(source_vt.struct_size as usize, size_of::<KopawNodeVTable>());
    std::ptr::copy_nonoverlapping(
        d.vtable as *const u8,
        &mut vt as *mut KopawNodeVTable as *mut u8,
        copy_size,
    );
    let core = &*(g as *const crate::graph::GraphCore);
    match core.add_node(
        desc_name(&*desc),
        d.user_data,
        &vt,
        d.outputs,
        d.inputs,
        d.queue_capacity,
        d.is_sink != 0,
        d.self_driven != 0,
    ) {
        Ok(id) => id,
        Err(_) => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_node_output(
    g: *mut KopawGraph,
    node: u32,
    port: u32,
) -> KopawOutput {
    let invalid = KopawOutput { id: 0 };
    if g.is_null() {
        return invalid;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    match core.node_output(node, port) {
        Some(o) => o,
        None => invalid,
    }
}

/// 连接：src 输出端口 → dst 节点（容量 cap 的有界队列）。
/// 每个输入端口至多一条入边；响应式节点单入边用 send，多入边必须提供
/// send_port；自驱动节点按端口拉取。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_connect(
    g: *mut KopawGraph,
    src: KopawOutput,
    dst_node: u32,
    dst_port: u32,
    cap: u32,
) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.connect(src, dst_node, dst_port, cap)
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_start(g: *mut KopawGraph) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.start()
}

/// 请求停止并等待全部节点线程退出。幂等。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_stop(g: *mut KopawGraph, timeout_ms: u32) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let _ = timeout_ms; // MVP：节点必须响应停止标志，不做超时强杀
    let core = &*(g as *const crate::graph::GraphCore);
    core.stop()
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_set_event_cb(
    g: *mut KopawGraph,
    cb: Option<
        unsafe extern "C" fn(user: *mut c_void, ev: KopawEventType, code: i32, msg: *const c_char),
    >,
    user: *mut c_void,
) {
    if g.is_null() {
        return;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.set_event_cb(cb, user);
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_state(g: *mut KopawGraph) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.state()
}

/// 节点向指定输出端口发射帧（阻塞式背压）。
/// 图停止时释放帧并返回 KOPAW_E_STOPPED。输出端口未连接时直接释放帧（丢弃）。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_emit(
    g: *mut KopawGraph,
    out: KopawOutput,
    frame: *mut KopawFrame,
) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.emit(out, frame)
}

/// 自驱动节点拉取：从其指定输入端口取一帧（所有权转移到调用方）。
/// 返回的帧可能带 EOS 标志；调用方负责 release。port=0 等价 kopaw_node_recv。
#[no_mangle]
pub unsafe extern "C" fn kopaw_node_recv_port(
    g: *mut KopawGraph,
    node: u32,
    port: u32,
    out_frame: *mut *mut KopawFrame,
    timeout_ms: u32,
) -> i32 {
    if g.is_null() || out_frame.is_null() {
        return KOPAW_E_INVALID;
    }
    *out_frame = std::ptr::null_mut();
    let core = &*(g as *const crate::graph::GraphCore);
    core.recv_port(node, port, &mut *out_frame, timeout_ms)
}

/// 自驱动节点拉取（单入边节点等价 recv_port 端口 0）。保留兼容。
#[no_mangle]
pub unsafe extern "C" fn kopaw_node_recv(
    g: *mut KopawGraph,
    node: u32,
    out_frame: *mut *mut KopawFrame,
    timeout_ms: u32,
) -> i32 {
    kopaw_node_recv_port(g, node, 0, out_frame, timeout_ms)
}

/// 调度模式：workers=0 → 每节点一线程（默认）；workers>0 → 共享队列+工作窃取
/// 线程池（单入边响应式节点以串行化 drain 任务执行；源/自驱动/多入边响应式
/// 节点仍各占一线程）。必须在 start 前调用。返回实际生效的 worker 数。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_set_workers(g: *mut KopawGraph, workers: u32) -> u32 {
    if g.is_null() {
        return 0;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.set_workers(workers)
}

/// 媒体时钟写入（音频汇聚节点在回调线程调用）
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_clock_set(g: *mut KopawGraph, media_us: i64) {
    if g.is_null() {
        return;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.clock.set(media_us);
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_clock_get(g: *mut KopawGraph) -> i64 {
    if g.is_null() {
        return 0;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.clock.now_us()
}

/// 时钟是否已被音频路径激活；未激活时视频渲染按墙上时钟节拍。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_clock_active(g: *mut KopawGraph) -> i32 {
    if g.is_null() {
        return 0;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.clock.active() as i32
}

/// 汇聚节点完成记账（P1：延迟记账协议）。
/// 引擎不再在 EOS 帧交付时自动计数——需要尾后处理（如音频环形缓冲排空）的
/// 汇聚节点在真正完成时自行调用；视频汇聚在最后一帧绘制后调用。
/// 仅汇聚节点有效；重复调用安全（只会触发一次 FINISHED）。
#[no_mangle]
pub unsafe extern "C" fn kopaw_node_sink_done(g: *mut KopawGraph, node: u32) -> i32 {
    if g.is_null() {
        return KOPAW_E_INVALID;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    core.sink_done(node)
}

/// 性能基线：导出每节点统计与链路占用（JSON）。
/// 返回写入 buf 的字节数；buf 为空或容量不足时返回所需字节数（含 NUL，取负值 -n）。
#[no_mangle]
pub unsafe extern "C" fn kopaw_graph_stats_json(
    g: *mut KopawGraph,
    buf: *mut c_char,
    cap: u32,
) -> i32 {
    if g.is_null() {
        return -1;
    }
    let core = &*(g as *const crate::graph::GraphCore);
    let s = core.stats_json();
    let need = s.len() + 1;
    if buf.is_null() || (cap as usize) < need {
        return -(need as i32);
    }
    std::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, need);
    need as i32
}

/// 打包 ABI 版本：高 16 位为主版本，低 16 位为次版本。
#[no_mangle]
pub unsafe extern "C" fn kopaw_abi_version() -> u32 {
    KOPAW_ABI_VERSION
}

/// 主机 ABI 信息；插件加载器使用它进行版本、能力和结构体大小协商。
#[no_mangle]
pub unsafe extern "C" fn kopaw_abi_info() -> KopawAbiInfo {
    KopawAbiInfo {
        struct_size: size_of::<KopawAbiInfo>() as u32,
        major: KOPAW_ABI_MAJOR,
        minor: KOPAW_ABI_MINOR,
        capabilities: KOPAW_ABI_CAPABILITIES,
    }
}

#[no_mangle]
pub unsafe extern "C" fn kopaw_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}
