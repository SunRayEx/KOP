# KOP 路线图

## 现状（2026-09）：M0/P1/P2 ✅ + P3-M1/M2.4/M3/M4 ✅

- ✅ 仓库骨架：CMake 超构建 + Corrosion + FetchContent 自包含依赖 + CI 友好预设
- ✅ kopaw-core（Rust）：图/队列/时钟/调度器，单元测试（所有权/停止/EOS/tee/
  多入边/池调度全覆盖）
- ✅ C ABI：cbindgen 生成 kopaw_abi.h（v5.2：版本/能力位、Handle 媒体地址、插件输出绑定、
  引用计数、DMA-BUF 字段、Vulkan external memory 帧类型、send_port 多入边虚表、
  drm_fourcc 和 sink-done 协议）
- ✅ C++ 节点：FFmpeg 解复用/解码/重采样，PortAudio 汇聚（主时钟），Vulkan 渲染
- ✅ kopaw-player：CLI 播放器，A/V 同步，`--kopms-bus` 零拷贝输出到 KOPMS 合成器
- ✅ KOPMS M1：Wayland/xdg-shell 最小服务端、nested OpenGL、wl_shm 生命周期和测试客户端
- ✅ KOPMS M3/M4：Vulkan GPU 场景合成、linux-dmabuf、explicit sync、帧回压（见下）

## P1：KOPAW 品质化 ✅（2026-08 完成）

- ✅ 帧池化：全局 FramePool（按 media_type+容量分桶，稳态解码输出零 malloc）；
  KopawFrame 引用计数（retain/release），媒体内存使用 `dma_buf_handle`，DMA-BUF
  fd 生命周期由最后一个 release 关闭
- ✅ tee 与多出边：同一输出端口可连多条边，引擎逐边 retain 广播、慢消费者整体背压；
  单入边约束保留（多入边混音属 P2 拉取式调度范畴）
- ✅ 硬解码：KOPAW_HWACCEL=auto|none|vaapi|cuda；探测 → 尝试 → 运行期优雅回退
  （get_format 二次协商返回软格式时动态切软解）。实测：HEVC 走 VAAPI 真硬解
  零回退；mpeg4 在 FFmpeg8 上运行期回退软解不中断播放
- ✅ OpenGL 后端：GL 3.3 core（glvnd 直链，无加载器依赖），双缓冲纹理 +
  letterbox + 垂直同步；`--backend opengl` / `KOPAW_BUILD_OPENGL=ON`
- ✅ letterbox + 窗口缩放：两后端统一 compute_letterbox；Vulkan 帧缓冲尺寸
  检测触发 swapchain/管线重建，最小化恢复
- ✅ 音频尾部修复：汇聚节点 EOS 记账改显式协议（kopaw_node_sink_done），
  音频在 ring 排空、全部样本真正播出后才触发 FINISHED
- ✅ 性能基线：每节点 delivered/busy_us/busy_max_us + 每链路 enqueued/dequeued/
  max_occ；kopaw_graph_stats_json 导出，player 退出时打印，
  KOPAW_STATS=1 时每 5s 周期打印

## P2：KOPAW 平台化 ✅（2026-09 完成）

- [x] P2.1 插件化节点：.so 动态加载、ABI 主/次版本与能力位协商、结构体校验、demo 节点和卸载测试
- [x] 多入边与 pull 式节点、工作窃取调度：ABI 5.2 增补 `send_port` 虚表与
  `KOPAW_CAP_MULTI_INPUT`；响应式节点可声明多入边（引擎单线程轮询各端口队列、
  按端口投递，EOS 逐端口终结）；自驱动节点多端口 `recv_port` 拉取补测试；
  工作窃取池调度（`KOPAW_SCHED=pool`）与多入边组合验证零泄漏
- [x] 硬解零拷贝：hw frames → DMA-BUF 导出 → Vulkan/KOPMS 导入。VAAPI 表面经
  `vaExportSurfaceHandle(DRM_PRIME_2)` 导出单对象双平面 NV12/P010
  （COMPOSED_LAYERS），帧携带 `drm_fourcc`/planes/引用计数，release 归还解码表面池；
  KOPAW Vulkan 后端按实际 format/modifier 建外部图像，用
  `VkSamplerYcbcrConversion` 完成有限范围 BT.601/709 并复用 `quad.frag`；
  `KopmsSinkNode` 可将原生 DMA-BUF 直接提交 BUS。 `--zero-copy on|off|auto`
  的 VAAPI 导出失败会回退系统内存路径；本地渲染端导入失败尚未运行时降级。
  CUDA（CUVID）原生导出依赖 EGL 互操作，留待后续
- [x] P2.3 网络输入与转码：RTSP/HTTP(S)、超时/断连处理、过滤、编码、封装和文件输出
- [x] P2.4 lavfi 桥接：视频/音频过滤器节点、player `--filter` 和 transcode 过滤链

## P3：KOPMS 启动（见 docs/kopms-design.md）

- [x] M1 协议骨架：wayland-server + xdg-shell + wl_shm，握手/提交/callback/断连有 smoke test
- [x] M2.1 资源发现：DRM connector/encoder/CRTC/primary-plane 选择和能力快照（只读）
- [x] M2 防护骨架：DRM/KMS、seat/session 与 libinput 绑定保护（输入注入仍禁用）
- [x] M2.2 导入防护：显式 DRM session、PRIME FD→GEM、framebuffer RAII、modifier/fence 校验
- [x] M2.3 atomic 边界：property 快照、TEST_ONLY/modeset/page-flip API 和事件等待
- [x] M2.4 物理直出启动骨架：logind TakeDevice、DRM fd 接管、atomic TEST_ONLY、nested 回退和 udev 恢复
- [x] KOPMS-S/KOPMS-C BUS2LAYER：版本/能力协商、CONTROL/MEDIA 双 lane、窗口状态 ACK、
  seqpacket + SCM_RIGHTS、帧 release 和事件循环接入
- [x] M3 KOPAW 帧池与零拷贝（2026-09）：
  - KopawFrame v5.2 统一生命周期接口：CPU / DMA-BUF / Vulkan external memory 三类后端，
    `drm_fourcc`、固定 plane 元数据、acquire fence、引用计数与 release 回调
    （OwnedFrame 新增 external_release 资源归还钩子）；
  - `vk_dma_export`：Vulkan 图像导出 DMA-BUF（LINEAR modifier，跨设备可导入）+
    sync-fence acquire fence；图像池随帧引用归零回收；
  - KOPMS `vk_scene`：vkImportMemoryFdKHR 导入 RGBA 与单对象双平面 NV12/P010、
    多窗口 GPU 合成、每窗口一帧回压、present/fence 完成后才释放。YUV 路径用
    RGB identity 的 YCbCr 采样提取平面，再由 `nv12.frag` 应用有限范围
    BT.601/709 矩阵；DRM 直出模式由 page-flip 完成事件驱动释放；
  - 全部路径不复用 M1 CPU wl_shm。
- [x] M4 KOPMS-Wayland 融合（2026-09）：
  - zwp_linux_dmabuf_v1（v1-v3）：格式/modifier 协商、params → wl_buffer；
  - wl_buffer → KOPMS Handle：dmabuf wl_buffer 直接进入与 BUS 帧同一 Vulkan 场景；
  - linux-explicit-synchronization（unstable v1）：set_acquire_fence 有界等待 +
    get_release fenced/immediate 回送；
  - CONTROL WINDOW_ATTACH 把窗口树/焦点/剪贴板/ownership 与 native DMA-BUF 帧绑定，
    焦点窗口置顶、场景网格布局；
  - 多窗口 GPU 合成与帧回压（每窗口一帧，超出即 DROPPED）。

### P3-M3/M4 协议与验证

- HELLO 新增能力位：Handle（0x10）、modifier（0x20）、explicit-sync（0x40）；
  32 字节消息头、MEDIA/CONTROL 双 lane、SCM_RIGHTS 机制不变（协议 minor 1.1）。
- 测试：`kopms-m34-protocol`（malformed FD、重复 release、跨 session handle、
  fence 超时、异常退出、WINDOW_ATTACH、能力门控）与 `kopms-vk-dmabuf-roundtrip`
  （Vulkan 导出 → 跨设备导入 → 像素逐一比对；无 DMA-BUF 能力时跳过），以及
  `kopms-vk-nv12-scene`（LINEAR NV12 DMA-BUF 导入 → 合成 → 回读有限范围
  BT.601 结果；无 DMA-BUF 能力时跳过）。
- 输入/输出持久压测 `kopms-input-soak`：输入管线（nested GLFW 真实输入 + soak
  自驱动脚本）→ 反馈客户端渲染点击标记/键入字符；实测 8 轮 40 点击/104 键入/
  103 移动 100% 投递，合成器 fd 无泄漏。
- 验证环境：Mesa llvmpipe（软件渲染）、Intel Iris Xe + NVIDIA MX450 双显卡
  （导出/导入/合成实测在 MX450 上通过）；vkms 路径经 drm_buffer 导入防护层覆盖，
  真机 KMS 直出需 logind 会话（`--direct-drm`）。
- 基准：`kopms-dmabuf-bench`（提交→释放端到端延迟、wire+导入耗时、fence 等待、
  CPU 占用、回压丢失）。实测（320x240，MX450，offscreen）：
  submit→release 平均 2.2ms / p99 5.9ms，wire+导入 0.49ms，导出上传 0.42ms，
  CPU 84.7%，回压 20 dropped / 20 accepted。

## 后续

- [ ] 硬解零拷贝跨后端：CUVID（CUDA）原生导出需 EGL/DMA-BUF 互操作；
  NV12/P010 之外的原生表面格式按需扩展导出器
- [ ] 在播放器启动时协商后端的 DMA-BUF/format/modifier 能力，并在本地 Vulkan
  导入失败时动态回退 CPU 路径
- [ ] 将色彩范围、矩阵、传递函数/HDR 元数据纳入帧与 BUS 契约；当前高度阈值
  只能覆盖有限范围 BT.601/709
- [ ] KOPMS DRM 直出的合成结果 page-flip 真机验证（vkms / logind 环境）
