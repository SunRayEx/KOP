# KOPAW 设计（Kongar Pipe Audio/Video Wire）

KOP 的音视频管线子系统，第一优先级。本文记录 MVP 已实现的设计与后续演进方向。

## 1. 模型

- **图（Graph）**：节点 + 有向链路。节点实现（C++）经 `KopawNodeDesc` 注册；
  引擎（Rust）持有全部链路与队列。
- **节点**：三种形态
  - **源节点**（无入边）：引擎线程运行其 `vtable.run`，主动生产（demuxer）；
  - **响应式节点**：引擎线程从其唯一入边队列拉帧，调 `vtable.send`（解码器、音频汇聚）；
  - **自驱动节点**（`self_driven`）：引擎线程运行 `run`，节点用 `kopaw_node_recv` 拉取
    并自持节奏（视频渲染节点）。
- **帧（KopawFrame）**：跨 ABI 的独占所有权媒体帧。MVP 布局：
  视频 = RGBA8 打包（stride=width×4）；音频 = f32 交错（48kHz/立体声）。
  `pts`/`dts` 统一为微秒。媒体内存由 `uint64_t dma_buf_handle` 标识：CPU 帧
  只保存当前进程的地址别名，DMA-BUF 帧保存本地句柄；EOS 以帧标志在数据面传递
  （与数据严格有序）。
- **队列**：每条链路一个有界队列（crossbeam bounded + 停止标志轮询）。
  发送阻塞 = 天然背压；停止/析构时滞留帧统一归还生产者，**零泄漏**
  （kopaw-core 单元测试覆盖：自然结束/中途停止/自驱动/未连接丢弃）。

## 2. 时钟与 A/V 同步

```
音频汇聚节点 PortAudio 回调线程
    └─ 按已消费帧数上报媒体位置 → kopaw_graph_clock_set()
                                     └─ MediaClock（互斥保护 + 单调插值）
视频渲染节点（自驱动）
    └─ 每帧取 target=pts，与 clock.now() 比较
        · 提前 0.8ms 内 sleep 等待（分片 2ms，保持停止响应）
        · 时钟未激活（无音频流/未起播）→ 以首帧为原点的墙上时钟节拍
```

音频为主时钟（对齐 PipeWire/Weston 惯例）；音画偏差由视频侧单向量调整。
注意：音频汇聚节点构造早于图创建，player 必须在入图后回填图句柄与节点 id
（P1 期间修复的静默失效问题——此前主时钟上报从未真正生效）。

## 3. P1 引入的机制

**帧池化与引用计数**（`kopaw/modules/frame.hpp`）
- `OwnedFrame` 携带原子引用计数；`FramePool` 全局按 (media_type, 容量) 分桶回收，
  解码输出稳态零 malloc（单桶上限 32，超出直接销毁）；池生命周期 = 进程，
  节点销毁后滞留帧安全回收。
- tee/多出边投递时引擎逐边 retain；帧 ABI 含 `memory_type`/`dma_buf_handle`/`dma_fd`
  （DMA-BUF 外部内存就绪：最后一个 release 关闭 fd；硬件导出路径仍待接入）。

**tee 与多出边**
- 同一输出端口可 connect 多条边；emit 逐边阻塞投递（慢消费者整体背压），
  投递第 i>0 条边前 retain；retain 缺失且多边 → 丢弃帧并返回 E_INVALID。
- 入边仍约束为单条（多入边混音与 pull 式调度属 P2）。

**P2 插件与过滤/网络路径**
- 插件 ABI 位于 `kopaw/abi/include/kopaw_plugin.h`：主/次版本、能力位、结构体大小和
  必需函数由 `PluginRegistry` 在 `dlopen` 后校验；有输出端口的节点还必须提供
  `bind_output`，由宿主在 `kopaw_graph_add_node` 后绑定实际句柄；`kopaw/plugins/demo/`
  提供可实际透传的 demo。
- `VideoFilterNode`/`AudioFilterNode` 将 FFmpeg `AVFilterGraph` 封装为普通 KOPAW 节点，
  player 可重复使用 `--filter video:<链>`/`--filter audio:<链>`。
- `kopaw-transcode` 支持本地文件与 RTSP/HTTP(S) 输入、lavfi、编码、封装和输出；网络
  I/O 通过 `AVIOInterruptCB` 区分主动停止、超时、EAGAIN 和断连。

**硬解码探测与回退**（`kopaw/modules/ffmpeg/hwaccel.cpp`）
- `KOPAW_HWACCEL=auto|none|vaapi|cuda`（默认 auto，顺序 vaapi→cuda）；
- 探测 = avcodec_get_hw_config 查 hw pix fmt + av_hwdevice_ctx_create；
- 运行期回退：解码器可能在 hw 初始化失败后以纯软格式列表重新协商 get_format，
  回调接受首个软格式并动态关闭硬解帧路径（mpeg4@FFmpeg8 实测走此路径）；
  HEVC/H.264 在 Intel TigerLake 上 VAAPI 真硬解直通（帧经
  av_hwframe_transfer_data 拉回 NV12 → sws → RGBA；零拷贝直通属 P2）。

**延迟记账（音频尾部修复）**
- 汇聚节点不再由引擎在 EOS 交付时自动计数，改为显式 `kopaw_node_sink_done`；
  音频在 PortAudio 回调排空环形缓冲、全部样本播出后才记账——FINISHED 触发时
  尾部不再截断；视频渲染在最后一帧绘制后记账。

**性能基线**
- 每节点：delivered / busy_us / busy_max_us（响应式节点 send 耗时，含下游背压）；
- 每链路：enqueued / dequeued / max_occ（近似峰值占用）；
- `kopaw_graph_stats_json` 导出 JSON；player 退出时打印，
  `KOPAW_STATS=1` 时每 5s 周期打印。

**渲染（两后端）**
- 统一 `compute_letterbox`：视频按纵横比居中，黑边由整窗 clear 填充；
- Vulkan：帧缓冲尺寸变化 → swapchain/管线重建（含最小化恢复）；
- OpenGL：GL 3.3 core（GL_GLEXT_PROTOTYPES 经 glvnd 直链），双缓冲纹理 +
  UNPACK_ROW_LENGTH 支持 stride，垂直同步，`--backend opengl` 选择。

## 3.1 P3-M3 零拷贝（ABI 5.1）

**KopawFrame 三类内存后端**
- `KOPAW_MEMORY_CPU`：地址别名，原 MVP 路径不变；
- `KOPAW_MEMORY_DMABUF`：外部 DMA-BUF，最后一个 release 关闭 fd；
- `KOPAW_MEMORY_VULKAN`（5.1 新增）：生产者 Vulkan 设备持有可导出 VkImage，
  `dma_buf_handle` 为本地 VkImage 句柄（仅同进程 ABI 消费有效），planes[] 携带
  导出的 DMA-BUF fd/stride/modifier 供跨进程导入；对应能力位
  `KOPAW_CAP_VULKAN_EXTERNAL`（0x100），引擎能力掩码扩至 0x1ff。
- `OwnedFrame` 新增 `external_ctx/external_release` 钩子：引用归零时先关闭全部
  外部 fd，再由生产者归还图像资源（导出器空闲池）——帧不回指节点、也不回指引擎。

**Vulkan 图像导出（`modules/render/vulkan/vk_dma_export.cpp`）**
- `VulkanDmabufExporter::export_cpu_frame()`：CPU RGBA → staging → 可导出 VkImage
  （DRM modifier LINEAR；无 modifier 扩展时退回 LINEAR tiling）→ 提交后经
  `vkGetFenceFdKHR` 导出 sync-fence 作为 acquire fence；
- 图像池按尺寸复用、上限即背压边界（池耗尽时导出失败，由上游队列消解）；
- 行距/偏移经 `vkGetImageSubresourceLayout`（modifier 图像用 MEMORY_PLANE aspect）。

**KOPMS 零拷贝汇聚节点（`modules/kopms/kopms_sink_node.cpp`）**
- 响应式汇聚：CPU 帧 → 导出 → `KopmsFrameDescriptor` 包装（retain/release 映射到
  KopawFrame 引用计数）→ BUS2LAYER 提交；
- 在飞上限（默认 3）构成生产端背压：send 阻塞泵 FRAME_RELEASE，图停止安全
 （500ms 有界超时后丢帧）；
- EOS 时排空全部在飞后才 `kopaw_node_sink_done` 记账；
- player 经 `--kopms-bus NAME [--kopms-window ID]` 启用；连接时完成 HELLO 能力
 协商并执行 CONTROL WINDOW_CREATE/WINDOW_ATTACH 绑定目标窗口。

**共享 Vulkan 装置（`common/src/vk_util.cpp`）**
- KOPAW 导出器与 KOPMS 场景共用的窗口无关 instance/device 装配：图形队列 +
  外部内存/DMA-BUF/sync-fence 扩展过滤、`vkGetMemoryFdKHR/vkGetFenceFdKHR`
  设备级命令解析（加载器不静态导出）。

## 4. MVP 数据流（kopaw-player）

```
test_media.mkv (mpeg4+mp2)
  demuxer ─┬─ 包帧 ─ video_decoder ─ RGBA 帧 ─ video_render（Vulkan/GLFW 窗口）
           └─ 包帧 ─ audio_decoder ─ f32/48k/2ch ─ audio_sink（SPSC ring → PortAudio）
```

- 解码输出在节点内完成标准化（sws→RGBA、swr→f32/48k/2ch），帧格式对下游统一；
- 视频渲染：双缓冲乒乓纹理 + 动态渲染（Vulkan 1.3 dynamic rendering）+ FIFO 垂直同步；
  着色器仅做全屏采样；
- 音频：SPSC 无锁环形缓冲（`kop/spsc_ring.h`），150ms 预缓冲后起播，
  回调内零锁零分配；欠载计一次告警。

## 5. 停止与生命周期

- 自然结束：全部汇聚节点各入边收到 EOS → 引擎置 FINISHED → 事件回调通知宿主；
- 主动停止（`--duration`、窗口关闭、Ctrl-C 后续支持）：宿主调 `kopaw_graph_stop` →
  引擎先发节点级 stop 提示（Pa_Abort 等）→ 置 STOPPING 解除全部队列阻塞 → join →
  清点归还滞留帧 → free 时逐节点 `destroy`。
- 契约红线见 `docs/architecture.md` 第 4 节（所有权违规是本 MVP 调试中实际踩过的坑，
  已用单元测试 + 端到端运行双重验证）。

## 6. 已知限制（P1 刻意取舍；P3-M3 已消解零拷贝项）

- 单入边（tee 广播已支持，多入边混音与 pull 式节点属 P2 演进）；
- 帧池对解码输出生效；demuxer 包帧仍为每包分配（体积小）；
- ~~硬解帧经 av_hwframe_transfer_data 拉回系统内存~~：软解→Vulkan 导出→KOPMS
  的零拷贝链路已通（P3-M3）；硬解器的原生 DMA-BUF 导出（VAAPI/CUVID）仍待接入；
- 图内仍未提供通用编码器节点；编码/封装目前集中在 `kopaw-transcode` 工具。
- 网络输入已在 transcode 路径可用，复杂协议重连策略和生产级 jitter buffer 仍待演进。

## 7. 演进路线

见 `docs/roadmap.md`（剩余 P2：多入边/工作窃取调度和硬解器原生 DMA-BUF 导出；
P3 后续：多平面 YUV 导入、swapchain 流水化、DRM 直出真机验证）。
