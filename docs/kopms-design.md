# KOPMS 设计（Kongar Pipe Monitor Server）

> **状态：P3-M1/M2.4 已完成；M3/M4 零拷贝与 Wayland 融合已落地。**
> M1 的 nested `wl_shm` 路径只用于协议验证，KOPAW 帧与 dmabuf wl_buffer 走
> Vulkan GPU 场景，两条路径永不交叉。

## 路线决策

**Wayland 兼容合成器**：KOPMS 实现 Wayland 协议服务端（合成器），现有 Linux 图形应用
直接连接运行，生态兼容最好；对标 wlroots/weston 的分层，而不是另起炉灶的自有协议
（自有协议再兼容的生态成本已被 Wayland 历史证明不合理）。

## 架构草图

```
┌─────────────────────────────────────────────┐
│ KOPMS（合成器进程）                           │
│  会话/输入层：libinput + seat/logind 集成      │
│  协议层：wayland-server + xdg-shell 等核心协议 │
│  合成核心（后续演进）                           │
│  M1 nested OpenGL / M2.4 DRM bootstrap 输出     │
├─────────────────────────────────────────────┤
│ DRM/KMS（直接渲染） ←→ 现有 Wayland 会话      │
└─────────────────────────────────────────────┘
```

## KOPMS-S/C 与 BUS2LAYER

KOPMS-S 和 KOPMS-C 使用独立于 Wayland 的本地 `AF_UNIX/SOCK_SEQPACKET` socket。
它们共享 compositor 的 `wl_event_loop`，因此 KOPAW 帧提交不需要额外通信线程：

```
KOPAW producer
    │ KopmsFrameDescriptor（DMABUF planes + acquire fence）
    ▼
KOPMS-C API ── BUS2LAYER: seqpacket + SCM_RIGHTS ──► KOPMS-S session
                                                        │
                                                        ▼
                                             compositor frame sink
Wayland client ── Wayland protocol ────────────────────┘
```

BUS 层固定 32 字节 little-endian 消息头，包含 magic、主/次版本、类型、flags、
方向序列号、payload 字节数和 ancillary FD 数量。HELLO/HELLO_ACK 协商能力位与
上限；FRAME_SUBMIT 只传帧元数据，平面和 acquire fence 通过 FD 索引引用，禁止
把 raw 指针或 CPU `wl_shm` 内容塞入 BUS。KOPMS-S 在校验结构体大小、媒体类型、
平面/FD 索引和 fence 后发送 FRAME_RELEASE。

KOPMS-C 在提交前 retain descriptor，在收到 release 后调用 descriptor 的 release；
KOPMS-S 负责收到的 FD，直到同步 release 或 session 关闭才释放。服务端 handler
可以选择立即 Release 或 Retain，后者通过 `release_frame(session_id, frame_id)`
完成显式所有权转移。

### BUS2LAYER 双流

KOPMS 把控制状态和媒体帧放在同一可靠的 BUS 连接上，但在协议头中分为两个 lane，避免
窗口管理事件和高带宽帧提交共享一套语义：

```
D-Bus / desktop control producer
        │ 可替换的 KOP 控制适配器
        ▼
KOPMS-C CONTROL_COMMAND ── CONTROL lane ──► KOPMS-S
        │                                      │
KOPAW/GPU producer ── FRAME_SUBMIT ─ MEDIA ───┘
                                               │
                                  window tree / focus /
                                  clipboard / ownership
```

`CONTROL_COMMAND` 使用有界的结构化 payload，当前支持窗口创建/销毁、父子关系、输入焦点、
剪贴板所有者和 ownership 状态，并以 `CONTROL_ACK` 返回请求序列、状态码和 generation。
控制状态在 session 关闭时清理；父级循环、越权操作和带活动子窗口的销毁会返回冲突或权限错误。
当前仓库只提供 KOP 控制适配边界，不把 libdbus 作为硬运行时依赖；D-Bus 方法到该 payload
的映射由上层适配器负责。

`FRAME_SUBMIT`/`FRAME_RELEASE` 始终使用 MEDIA lane。两条 lane 共享单调递增的端点序列号、
HELLO 能力协商和 `SCM_RIGHTS` 传输，但控制 payload 不携带 FD，媒体 payload 只通过 FD 索引
引用 DMA-BUF/fence。这使 KOPMS-Wayland 可以继续在同一 `wl_event_loop` 中服务 Wayland 客户端，
同时让 KOPAW 的 native frame sink 走独立的 GPU 帧生命周期。

媒体帧不在 BUS 中传裸指针。`KopawFrame` 和 `KopmsFrameDescriptor` 使用
`uint64_t dma_buf_handle`：SINGLE 的 CPU 句柄只是当前进程地址别名；HYBRID 的句柄
在服务端读取 `SCM_RIGHTS` 后由本地 DMA-BUF FD 物化。客户端的数字 FD 不能跨进程复用，
服务端只在保持对应 FD 生命周期期间暴露该句柄。

compositor 默认发布 `<wayland-socket>.bus`，也可以用 `--bus-socket NAME` 或
`KOPMS_BUS_SOCKET` 指定。当前 handler 仅验证 DMA-BUF 生命周期并立即 release，
nested OpenGL 仍只显示独立的 `wl_shm` 路径；真正的 Vulkan/DRM 导入、fence 等待
和场景合成属于后续 M3。

`KOPMS_GPU_MODE=SINGLE|HYBRID` 在 compositor 启动时解析。缺省为 SINGLE，非法值
回退并记录警告；模式只改变句柄解释和校验策略，不改变 BUS2LAYER 的消息头、ring 或 FD
索引编码。M2.1 完成只读资源发现：KOPMS 复制 connector、encoder、CRTC、mode 和
primary plane 元数据，并用兼容性算法选出一个 connected output。M2.4 的 direct-DRM
模式在授权 fd 上创建黑色 dumb bootstrap buffer，完成 atomic `TEST_ONLY` 后执行初始
modeset；M3 之后合成结果另经 `DrmDirectOutput::present_dmabuf()` 走
AddFB2 + atomic page-flip，flip 完成事件到达后场景才释放参与合成的帧。

与 KOPAW 的协同点（也是混编架构的收益）：

1. **共享渲染后端**：KOPAW 的 `IRenderBackend`（Vulkan 优先）即 KOPMS 的场景渲染基座；
2. **共享缓冲语义**：KOPAW 帧池化后引入 DMA-BUF 导出，KOPMS 合成器可零拷贝合成
   管线帧（录像、画中画等场景）；
3. **时钟体系复用**：KOPAW 的媒体时钟与合成器的呈现时钟统一在 vkQueuePresent 节拍上。

## P3-M1 已实现

- `kopms-compositor` 发布 `wl_compositor`、`wl_shm`、`wl_output`、`wl_seat` 和
  `xdg_wm_base` 的最小可用集合；嵌套输出使用 GLFW + OpenGL 3.3。
- `KOPMS-Wayland` 与 KOPMS-S 共用 compositor 事件循环：Wayland 兼容面负责协议对象和
  `wl_shm` 验证，KOPMS-S/C 负责 BUS2LAYER 的控制与 native 媒体面；两者不共享 CPU buffer
  的所有权契约。
- `xdg_surface.configure` 使用服务端生成的 serial，客户端必须先
  `ack_configure`；无效 serial 会触发协议错误。
- 客户端提交的 `ARGB8888/XRGB8888` `wl_shm` 内容在 compositor 内同步复制到纹理，
  复制完成后发送 `wl_buffer.release` 并完成 `wl_surface.frame` callback。
- 客户端退出触发正常断连日志，不把连接关闭误报为 compositor 错误。
- `kopms-test-client` 和 `kopms-protocol-smoke` 覆盖握手、提交、callback 和断连；
  `kopms-bus-test` 覆盖 lane、控制 ACK、窗口层级冲突和媒体帧 release；
  `kopms-dmabuf-client-test` 用真实 Wayland 客户端进程跨 socket 验证
  dmabuf→场景→release/explicit-sync 全链（双轮、fenced release）。
- `wl_seat` 的 get_pointer/get_keyboard/get_touch 返回真实资源对象（客户端请求
  新对象时必须应答，否则 libwayland 直接断开连接）。
- **输入事件管线**（`compositor/input_state.cpp`）：nested 输出的 GLFW 真实输入
  （光标/鼠标按键/键盘）与 soak 自驱动共用同一管线，按 wl_pointer/wl_keyboard
  协议投递（enter→motion→button→frame；键盘 keymap（xkbcommon evdev/us）→
  modifiers→key，v5+ 指针每批事件带 frame）。指针聚焦 = 最近提交内容的 xdg 主
  surface 的 1:1 内容矩形；键盘焦点跟随点击。键码映射为真实 QWERTY evdev 布局
  （`keymap_table.hpp`，与反馈客户端共享——字母键码非字母表序）。
- **输入/输出持久压测**（`kopms-input-soak`）：合成器以 KOPMS_INPUT_SOAK 自驱动
  多轮"移动扫掠+点击+键入"脚本 → `kopms-input-client` 渲染点击标记/键入字符
  （GUI 反馈回路闭合）→ 测试核对注入/接收计数（实测 8 轮 40 点击/104 键入/
  103 移动 100% 投递）与合成器 fd 稳定性（无泄漏）。
- 真实客户端可达的全部请求必须有处理器（xdg_toplevel 状态类、wl_surface 的
  region/scale/transform、wl_region.add/subtract、wl_pointer.set_cursor、
  xdg_popup/xdg_positioner、各 destroy）：NULL 处理器触发 libwayland
  `wl_abort`，整个合成器进程终止——见 architecture.md 红线第 8 条。
- 兼容性实测：`wayland-info`、`kopms-test-client`（232 帧动画）、GTK3 demo
  ——真实第三方应用连接、xdg 握手、ARGB8888 shm 内容连续提交渲染、buffer
  release 与 frame callback 全链稳定。GL 加速（EGL dmabuf v4 feedback）与
  keyboard keymap 属后续里程碑。

## P3-M2.1 资源发现与防护边界

- `DrmKmsGuard::discover()` 只打开指定 primary node，复制 connector/encoder/CRTC/plane
  值快照，并选择完整的 connector → encoder → CRTC → primary-plane 链路；`--probe-drm` 或
  `KOPMS_PROBE_DRM=1` 不会取得 DRM master、修改 connector 或执行 modeset。
- 已连接 connector 优先于 unknown connection；首选 mode 优先；没有兼容 primary plane
  时拒绝选择，避免把错误延迟到 DMA-BUF 导入阶段。
- 快照不携带 libdrm 资源指针或 fd，方便后续加入 session/seat 生命周期，也让选择逻辑可
  在无 GPU 的 CI 中测试。
- `LibinputGuard` 通过 `--enable-input` 或 `KOPMS_ENABLE_INPUT=1` 明确启用，
  绑定 `seat0`（可用 `--seat`/`KOPMS_SEAT` 覆盖），当前只排空事件，不向客户端注入输入。
- 缺少 libdrm/libinput 时仍可编译，启动路径会记录降级原因并保持 nested 输出。

## P3-M2.2 DMA-BUF 导入边界

- `DrmKmsSession` 将 DRM fd 的 open、显式 `acquire_master`、`drop_master` 和 close 分开；
  打开 session 不会隐式抢占 DRM master，调用方必须先获得 seat/logind/seatd 授权。
- session 在自己的 DRM file description 上单独启用 atomic client capability；不能把另一个
  fd 的 capability 快照当作当前 session 的状态。
- `DrmDmabufImportRequest` 只借用输入 DMA-BUF/fence fd；`import_dmabuf_frame()` 先校验
  media type、尺寸、plane、primary-plane 格式、stride、modifier 和 fence，再执行
  `PRIME_FD_TO_HANDLE` 与 `drmModeAddFB2*`。GEM handle 和 framebuffer 由
  `DrmImportedBuffer` 按引用生命周期释放。
- explicit sync_file fence 使用有界 `poll()` 等待；timeline fence 暂不伪装成已支持。
  importer 不关闭借用 fd，server 必须在 frame release 前保持它们有效。
- 该模块目前是可测试的导入防护层，尚未在 compositor handler 中启用；M2.4 的 bootstrap
  buffer 只验证输出生命周期，不把 KOPAW DMA-BUF 直接提交到物理输出。
- `DrmKmsSession::test_modeset()` 使用 `DRM_MODE_ATOMIC_TEST_ONLY`；只有显式调用
  `modeset()` 后通常才调用 `page_flip()`；后者通过 DRM event fd 等待完成回调，并阻止
  在 event 未完成时释放 master。

## P3-M2.3 Atomic transaction 边界

- 资源快照会复制 connector 的 `CRTC_ID`、CRTC 的 `ACTIVE/MODE_ID` 和 primary plane
  的 framebuffer、CRTC/source geometry property id；缺少任一必需 property 时不会生成
  可提交的 atomic output。
- `DrmKmsSession::test_modeset()` 生成 mode blob 并使用 `DRM_MODE_ATOMIC_TEST_ONLY`；
  `modeset()` 使用 `ALLOW_MODESET`，`disable_output()` 先关闭 CRTC/plane 再释放
  framebuffer，`page_flip()` 只更新 primary plane 的 `FB_ID`，并通过 `drmHandleEvent()`
  转发完成回调。
- 所有提交 API 都要求当前 session 已打开、在自己的 fd 上启用 atomic capability 并已
  显式取得 DRM master。它们只由显式 direct-DRM 启动路径调用，默认 nested compositor
  不会取得 master；本机仍未进行真实 GPU/KMS 验证。

## P3-M2.4 物理直出与会话恢复

- `SeatSession` 优先绑定当前进程的 systemd-logind session，校验 requested seat 和 active
  状态，并通过 `TakeDevice(major, minor)` 获取授权 DRM fd；`ReleaseDevice` 在停止时释放。
- `DrmKmsSession::open_fd()` 接管授权 fd，`DrmKmsGuard::discover_fd()` 在同一个 file
  description 上读取资源和 atomic property，避免重新按路径打开一个未经 seat 授权的节点。
- `DrmDirectOutput` 是 compositor 的显式直出入口。只有 seat active、DRM/KMS 资源完整、
  atomic property 完整、bootstrap framebuffer 创建成功且 `TEST_ONLY` 通过后才执行 modeset。
  bootstrap buffer 是内部黑色 dumb buffer，不来自 Wayland `wl_shm`，也不承载 KOPAW 帧。
- 默认启动不打开 direct DRM。`--direct-drm`/`KOPMS_DIRECT_DRM=1` 才启用；没有 logind
  session 时必须额外指定 `--allow-unmanaged-drm` 或 `KOPMS_ALLOW_UNMANAGED_DRM=1`，否则
  保持 nested 输出。该退化选项只适用于明确管理的 kiosk/test 环境。
- logind `PauseDevice`、`ResumeDevice`、`gone` 和 udev DRM add/change/remove 会驱动物理
  输出状态机。暂停、撤销、资源重新枚举失败或运行期 DRM 错误都会先关闭旧 framebuffer/
  master，再切换到 nested；设备重新出现后重新走授权、资源发现和 TEST_ONLY 流程。
- udev 监听通过 `--watch-drm`/`KOPMS_DRM_HOTPLUG` 启用；direct 模式默认启用。缺少
  libudev/systemd/libdrm 时构建仍成功，并记录原因后回退。

## P3-M3 帧接口边界（设计约束，实现见下节）

- 帧池化 + 外部内存（DMA-BUF）导入导出；
- Vulkan 渲染抽象里补齐 DRM/KMS swapchain 路径（目前仅窗口系统 surface）；
- Rust 侧合成核心的并发模型验证（可从 kopaw-core 的调度器演进）。

`kopms/platform/frame_bridge.h` 定义独立于 `wl_shm` 的 `KopmsFrameDescriptor`：

- `struct_size`/`version` 为扩展入口（当前 descriptor version=2）；`dma_buf_handle` 是
  唯一的媒体内存标识。DMA-BUF 描述包含每平面的 fd、offset、stride 和 modifier；同步
  对象包含 fence 类型、fd 和 timeline value。
- descriptor 通过 `retain`/`release` 管理引用，`FrameLease` 提供 RAII 转移；消费者不得
  自行关闭 DMA-BUF 或 fence fd。
- descriptor 的 CPU 地址别名只服务于 SINGLE 进程内 ABI 校验和未来工具，M1 compositor
  不读取它们；M3/M4 的 Vulkan → 合成器路径必须使用 HYBRID DMA-BUF/同步栅栏接口，不能
  复用当前 CPU `wl_shm`。

## P3-M3/M4 零拷贝合成（已实现）

数据面（两条入口，同一 GPU 场景）：

```
KOPAW 管线（kopms_sink 节点）
    CPU RGBA → VulkanDmabufExporter（vk_dma_export）
        导出 VkImage：LINEAR modifier DMA-BUF + sync-fence（KOPAW_MEMORY_VULKAN）
    VAAPI 原生解码帧（NV12/P010）
        → 直接复用 DMA-BUF planes（无需 RGBA 中转或二次导出）
    BUS2LAYER FRAME_SUBMIT（planes/fence 经 SCM_RIGHTS）
        → KOPMS-S handler：窗口绑定（WINDOW_ATTACH 或隐式窗口）
        → VulkanScene::submit（vkImportMemoryFdKHR + fence 有界等待）
Wayland 客户端
    linux-dmabuf params → wl_buffer → commit
        → DmabufBridge::buffer_view → 同一 VulkanScene::submit
        → explicit sync：set_acquire_fence 消费 / get_release 回送
VulkanScene::render（每帧）
    窗口树布局（CONTROL 快照 + 焦点置顶 + 网格 letterbox）
    → 合成 → present（windowed）或 render fence（offscreen）
    → GPU 完成后才逐帧释放（BUS FRAME_RELEASE / wl_buffer.release）
DRM 直出模式
    render 不等待不释放 → export_composite_dmabuf（等 fence）→ AddFB2
    → atomic page-flip → flip 完成事件 → complete_direct_present() 释放
```

生命周期与回压契约：

- 每个场景窗口同时在飞一帧；上一帧未呈现前新帧被拒收，handler 以
  `KOPMS_FRAME_RELEASE_DROPPED` 立即释放（帧回压信号，生产端减速或丢帧）。
- BUS 帧：handler 返回 Retain，服务端持有 fd；场景在 present/fence 完成后回调
  `release_frame(session, frame_id)`，服务端此刻才关闭 fd 并发 FRAME_RELEASE。
- Wayland buffer：场景释放回调发送 `wl_buffer.release`，explicit-sync 的
  `get_release` 请求按最近一次合成 fence 回 `fenced_release`（合成未发生则
  `immediate_release`）。
- KOPAW 侧 `KopmsSinkNode`（`--kopms-bus`）：导出图像池上限即在飞上限，send 阻塞
  泵 FRAME_RELEASE 形成生产端背压；CPU RGBA 使用导出器，带 `drm_fourcc` 的
  `KOPAW_MEMORY_DMABUF` 帧直接封装提交；有界 500ms 超时防止图停止时挂死。

**多平面 YUV 导入（P2）**

- `VulkanScene::submit` 支持单对象、双平面 NV12（8-bit）和 P010（10-bit）DMA-BUF；
  描述符当前要求两个平面 fd 相同。格式、planes、offset、stride 和 modifier 仍经
  `KopmsFrameDescriptor`/Wayland dmabuf 的原有入口传递。
- Vulkan 的 YCbCr conversion 以 RGB identity 模型完成平面提取与色度上采样；
  `nv12.frag` 再按 push constant 的位深及高度（≥720 为 BT.709，否则 BT.601）
  应用有限范围 YCbCr→RGB 矩阵。它与 KOPAW 本地渲染器的“固定功能完成完整
  BT.601/709 转换、复用 `quad.frag`”不同，不能把两者的管线和色彩元数据能力混为一谈。
- YUV 导入需要场景设备具备 YCbCr conversion；有 modifier 时还依赖 DRM modifier
  导入能力。acquire sync-fence 在导入前有界等待；VAAPI 导出帧已由生产端
  `vaSyncSurface` 同步，因此携带 `KOPAW_SYNC_FENCE_NONE`。

能力协商（协议 minor 1.1，32 字节头/双 lane/SCM_RIGHTS 不变）：

- `KOPMS_PROTOCOL_CAP_HANDLE_FRAMES`：KOPAW_MEMORY_VULKAN 帧端到端可用；
- `KOPMS_PROTOCOL_CAP_MODIFIERS`：非零 modifier 平面必须协商；
- `KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC`：acquire fence 必须协商；
- `KOPMS_CONTROL_WINDOW_ATTACH`（operation 7）：owner 会话把窗口绑定为自己的
  native 帧目标，会话断开自动解绑。

已知边界：多平面图像目前只接受单对象双平面 NV12/P010；色彩范围、矩阵和 HDR
元数据尚未通过 descriptor/BUS 传递。窗口模式 swapchain 为串行呈现（M4 验证基线）；
DRM 直出的真机 flip 验证需要 logind 会话或 vkms。

## 验证

```bash
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
ctest --test-dir build/relwithdebinfo --output-on-failure
```

BUS2LAYER 与 M3/M4 协议自测不依赖 X11/GPU（Vulkan 回环测试无 DMA-BUF 能力时
以 77 跳过）：

```bash
./build/relwithdebinfo/kopms/kopms-bus-test
./build/relwithdebinfo/kopms/kopms-m34-protocol-test
./build/relwithdebinfo/kopms/kopms-vk-dmabuf-test
./build/relwithdebinfo/kopms/kopms-vk-nv12-scene-test
```

`kopms-vk-nv12-scene-test` 创建实际的 LINEAR NV12 DMA-BUF，经与 BUS 相同的
`VulkanScene` 导入/合成路径回读 BT.601 有限范围结果；没有 DMA-BUF 能力的驱动以
退出码 77 跳过。

零拷贝基准（五项指标，JSON 输出）：

```bash
./build/relwithdebinfo/kopms/kopms-dmabuf-bench [frames]
```

在 X11 nested 环境手工运行（KOPAW 帧 → Vulkan 场景零拷贝合成，`--scene-window`
弹出场景呈现窗口）：

```bash
env -u WAYLAND_DISPLAY DISPLAY=:0 \
  ./build/relwithdebinfo/kopms/kopms-compositor kop-0 30 --scene-window
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv \
  --no-audio --kopms-bus kop-0.bus --duration 8
```

在 X11 nested 环境手工运行（M1 wl_shm 协议验证路径，保持不变）：

```bash
env -u WAYLAND_DISPLAY DISPLAY=:0 \
  ./build/relwithdebinfo/kopms/kopms-compositor kop-0 15
WAYLAND_DISPLAY=kop-0 ./build/relwithdebinfo/kopms/kopms-test-client 5
```

## 后续里程碑

- 色彩范围、矩阵、传递函数/HDR 元数据的 descriptor/BUS 协商；KOPMS YUV 路径
  仍使用 RGB identity + shader 矩阵，后续可与 KOPAW 的固定功能转换策略统一；
- 窗口模式 swapchain 流水化（多帧 in-flight）与 presentation-time 反馈；
- DRM 直出合成结果的真机 page-flip 验证（vkms / logind 双显卡 PRIME）；
- KOPMS-S/C 的 descriptor 与 release 契约保持为 Vulkan/DRM 的上游边界。
