# KOPMS — Kongar Pipe Monitor Server

KOP 显示服务器主体，目标是替代 Xorg 的传统架构。
**当前状态：P3-M1、P3-M2.1-M2.4 边界、KOPMS-S/KOPMS-C BUS2LAYER 以及 P3-M3/M4
零拷贝合成（Vulkan 场景、linux-dmabuf、explicit sync、帧回压）已实现。**
KOPAW 仍是系统的第一优先级。

## 技术路线（已决策）

- **Wayland 兼容合成器**：实现 Wayland 协议服务端，现有 Linux 图形应用可直接连接运行；
  参考 wlroots / weston 的架构分层。
- M1 提供 `xdg-shell`、`wl_shm`、frame callback 和 nested OpenGL 输出，测试客户端位于
  `clients/test_client.cpp`。
- KOPMS-S/KOPMS-C 使用 `<wayland-socket>.bus` 的 Unix `SOCK_SEQPACKET`；HELLO 能力协商、
  CONTROL/MEDIA 双 lane、控制 ACK、DMA-BUF/fence FD 传递和 FRAME_RELEASE 位于 `protocol/`
  与 `bus2layer/`。
- CONTROL lane 承载窗口创建/销毁、父子关系、焦点、剪贴板所有者和 ownership 状态；D-Bus
  只需要实现到 `KopmsControlCommandPayload` 的适配，不是当前构建的硬依赖。
- 媒体描述中的 `dma_buf_handle` 是进程内句柄：SINGLE 的 CPU 帧使用地址别名，HYBRID
  的 DMA-BUF 使用服务端由 `SCM_RIGHTS` 物化的本地 FD。客户端发送的数字 FD 不会直接
  在服务端解引用。
- `KOPMS_GPU_MODE=SINGLE|HYBRID` 在 compositor 初始化时锁定模式；缺省为 SINGLE，非法
  值记录警告并回退。两种模式共享 BUS2LAYER 的 ring/解析逻辑。
- native 帧入口（BUS `FRAME_SUBMIT` 与 Wayland linux-dmabuf `wl_buffer`）统一进入
  `platform/vk_scene.cpp` 的 Vulkan GPU 场景：DMA-BUF 导入、多窗口合成、每窗口一帧
  回压，present/fence（直出为 page-flip）完成后才释放。不会复用 M1 的 CPU
  `wl_shm` buffer。
- BUS 入口支持 RGBA 与单对象双平面 NV12/P010 DMA-BUF；KOPAW 的 VAAPI 原生帧可由
  `KopmsSinkNode` 直接提交。YUV 路径用 YCbCr conversion 提取/上采样平面，
  `nv12.frag` 根据帧携带的 matrix/range 显式完成 BT.601、BT.709 或 BT.2020 NCL
  转换，不按高度猜测色彩。Wayland linux-dmabuf bridge 当前公布 RGBA 格式；
  它尚未定义色彩管理 tail，YUV 原生帧应走 BUS。完整约束见 `docs/kopms-design.md`。
- `--probe-drm` 会枚举 connector/encoder/CRTC/primary plane，并报告 atomic/universal-plane
  能力；它不会取得 DRM master、执行 modeset 或提交 page-flip。`--enable-input` 只绑定
  seat 并排空事件，不注入输入。
- 资源发现采用值快照和纯选择算法；没有权限、没有 DRM 节点或没有完整输出链路时，
  compositor 保持 nested 输出并记录原因。
- `platform/drm_buffer.*` 提供显式 DRM session、DMA-BUF `PRIME_FD_TO_HANDLE`、
  `DRM framebuffer` RAII、modifier 校验和 acquire fence 等待；M3 起
  `DrmDirectOutput::present_dmabuf()` 用它把 Vulkan 场景的合成结果 AddFB2 并执行
  atomic page-flip，flip 完成事件后才释放场景帧。
- `DrmKmsSession` 在自己的 DRM fd 上单独协商 atomic capability；`test_modeset()`、
  `modeset()` 和 `page_flip()` 都要求 session 已取得 master、资源 property 完整且
  framebuffer 有效。
- `--direct-drm` 显式启用物理输出启动骨架：通过 systemd-logind `TakeDevice` 接管授权
  fd，在同一个 fd 上完成资源发现、黑色 dumb bootstrap framebuffer、atomic `TEST_ONLY`
  和初始 modeset；默认仍使用 nested。没有 logind 的 kiosk/test 环境必须显式加上
  `--allow-unmanaged-drm`。
- direct 输出收到 logind pause/revoke 或 DRM udev remove/change 后会撤销当前输出并回退
  nested；设备重新出现时重新执行 seat 授权和 atomic 校验。`--watch-drm` 可单独打开
  udev 监听，direct 模式默认打开。
- probe 日志还会报告 atomic property 是否完整；当前机器没有 `/dev/dri` 时只保留
  nested 输出，真实物理直出需要显式接入且不能跳过 seat 生命周期。
- M3/M4 使用 `platform/frame_bridge.h` 的 `dma_buf_handle`/DMA-BUF/fence/retain/release
  契约与 `compositor/dmabuf_bridge.cpp` 的 linux-dmabuf/explicit-sync 服务端实现，
  不能复用 M1 的 CPU `wl_shm` 路径。完整边界见 `docs/kopms-design.md`。

## 色彩、HDR 与兼容性契约

KOPAW ABI 5.3 的 `KopawColorMetadata` 会随 `KopawFrame`、
`KopmsFrameDescriptor` 以及 BUS `FRAME_SUBMIT` 传递：`range`（limited/full）、
`matrix`（BT.601/709/2020 NCL）、`transfer`、`primaries`、`chroma_location`，
以及可选 mastering-display/MaxCLL/MaxFALL HDR side data。BUS 协议 minor 1.2
在 160 字节基础前缀后追加 80 字节 tail；只有 HELLO 交集包含
`KOPMS_PROTOCOL_CAP_COLOR_METADATA` 时才发送 tail，旧 1.1 peer 仍可使用基础
前缀。所有扩展字段都由 `struct_size` 门控。

Vulkan 场景支持 limited/full、BT.601/709/2020 NCL 和 center/left/top/topleft
色度位置；unknown 色度位置按 center 处理。unknown range/matrix、BT.2020
constant-luminance、bottom/bottom-left 色度位置会拒收原生帧。transfer、primaries
和 HDR 数据目前只透传/记录；合成目标仍是 SDR，没有 HDR swapchain 或 tone mapping。

## DMA-BUF 能力协商与回退

BUS HELLO/HELLO_ACK 以能力交集锁定 session：DMABUF、FRAME_RELEASE、HANDLE_FRAMES、
MODIFIERS、EXPLICIT_SYNC 和 COLOR_METADATA 分别门控对应路径。播放器启动时先
初始化本地渲染后端，再把实际可用的 NV12/P010 format mask（含 YCbCr、DMA-BUF、
DRM modifier 和 `vkGetMemoryFdPropertiesKHR` 检查）与 KOPMS-S 的协商结果交给解码器；
启动 mask 只表示候选格式，实际 modifier、stride、平面布局和色彩值仍在每帧导入时
复核。

若 Vulkan 本地导入在创建/分配/绑定/视图任一步失败，RenderNode 丢弃该外部帧并
关闭解码器 native output，后续帧自动走 CPU。KOPMS 场景拒绝 BUS 原生帧时回送
`FRAME_RELEASE_REJECTED`，`KopmsSinkNode` 观察到后执行同一回退；回压拒收则是
`FRAME_RELEASE_DROPPED`。VAAPI exporter 当前只覆盖 NV12/P010，其他表面格式按
CPU 路径处理；CUDA/CUVID 不伪造 DMA-BUF exporter，原生导出仍需要 CUDA-EGL/
DMA-BUF interop，暂时使用软拷贝。

## 本地验证

```bash
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
ctest --test-dir build/relwithdebinfo --output-on-failure
```

在有 X11 nested 显示的环境运行：

```bash
env -u WAYLAND_DISPLAY DISPLAY=:0 \
  ./build/relwithdebinfo/kopms/kopms-compositor kop-0 15
WAYLAND_DISPLAY=kop-0 ./build/relwithdebinfo/kopms/kopms-test-client 5
```

物理直出必须显式打开；默认失败会回退 nested：

```bash
KOPMS_DIRECT_DRM=1 ./build/relwithdebinfo/kopms/kopms-compositor kop-0 15
```

没有 logind session 的受控 kiosk/test 环境才使用：

```bash
./build/relwithdebinfo/kopms/kopms-compositor --direct-drm --allow-unmanaged-drm kop-0 15
```

KOPMS-C 协议探测与 M3/M4 自测：

```bash
./build/relwithdebinfo/kopms/kopms-client "$XDG_RUNTIME_DIR/kop-0.bus"
./build/relwithdebinfo/kopms/kopms-bus-test
./build/relwithdebinfo/kopms/kopms-m34-protocol-test
./build/relwithdebinfo/kopms/kopms-vk-dmabuf-test   # 无 Vulkan DMA-BUF 时跳过
./build/relwithdebinfo/kopms/kopms-vk-nv12-scene-test # NV12 导入/合成/回读，无能力时跳过
./build/relwithdebinfo/kopms/kopms-dmabuf-bench     # 零拷贝基准（JSON）
```
