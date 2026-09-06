# KOP — Kongar Pipe

整合的音视频管线与显示服务器项目：**一套混编架构同时驱动媒体数据面与桌面合成面**。

- **KOPAW**（Kongar Pipe Audio/Video Wire）— 音视频管线，第一优先级，对标 PipeWire：
  Rust 图引擎 + C++ 节点（FFmpeg 解复用/解码、Vulkan/OpenGL 渲染、PortAudio 汇聚），
  支持插件 ABI、网络输入、转码封装、硬件解码回退、**VAAPI 原生 DMA-BUF 导出**与
  **Vulkan YCbCr 零拷贝渲染**。
- **KOPMS**（Kongar Pipe Monitor Server）— 显示服务器，Wayland 兼容合成器路线：
  wl_compositor/xdg-shell/wl_shm + linux-dmabuf + explicit synchronization，
  **Vulkan GPU 多窗口合成**、BUS2LAYER 原生帧协议（零拷贝/回压/释放契约）、
  输入事件管线（鼠标/键盘/keymap）、DRM/KMS 直出骨架与防护层。

技术栈：C/C++/Rust 按模块混编 · FFmpeg 8 · Vulkan 1.3（OpenGL 编译期可选）· PortAudio ·
libxkbcommon · CMake + Corrosion。平台：Linux（Wayland/X11 检测）。
许可证：**GPL-3.0-or-later**（见 [LICENSE](LICENSE)）。

```
┌────────────────────────── KOPAW（数据面） ──────────────────────────┐
│ demuxer → video_decoder → [filter] → 渲染节点 / KOPMS 零拷贝发布      │
│         → audio_decoder → PortAudio 汇聚（主时钟）                    │
│ Rust 图引擎：节点/链路/有界队列/tee 广播/池调度/媒体时钟/性能基线       │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ DMA-BUF（Vulkan 导出或 VAAPI 原生）
                    BUS2LAYER（seqpacket + SCM_RIGHTS，CONTROL/MEDIA 双 lane）
                                │
┌───────────────────────────────▼─────────────────────────────────────┐
│ KOPMS（合成面）：Vulkan GPU 场景 · linux-dmabuf · explicit sync       │
│ Wayland 兼容（xdg-shell/wl_shm/wl_seat）· 输入事件管线 · DRM 直出骨架 │
└──────────────────────────────────────────────────────────────────────┘
```

## 功能状态（详见 docs/roadmap.md）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0/P1 | 纵向切片 MVP、帧池化/tee/硬解回退/双渲染后端/letterbox/性能基线 | ✅ |
| P2 | 插件 ABI、多入边/工作窃取调度、网络输入/转码、lavfi、VAAPI 原生 DMA-BUF/YCbCr 导入 | ✅ |
| P3-M1/M2 | Wayland 协议骨架、DRM 资源发现/导入防护/atomic/直出恢复 | ✅ |
| P3-M3 | KOPAW 帧池与零拷贝（DMA-BUF/Vulkan external memory 三类后端、导出/导入、fence 后释放） | ✅ |
| P3-M4 | KOPMS-Wayland 融合（linux-dmabuf、explicit sync、窗口绑定、多窗口 GPU 合成、帧回压） | ✅ |
| 输入管线 | 鼠标/键盘事件投递（QWERTY evdev + xkbcommon keymap）+ 持久压测 | ✅ |
| 应用 SDK | KOP_AppSDK_Interface + KOPAW_Test / KOPMS_Test / KOP_Demo | ✅ |
| 后续 | CUVID 导出、色彩元数据/动态降级、swapchain 流水化、真机 KMS 直出 | ⬜ |

## 快速开始

```bash
# 0) 依赖体检（FFmpeg/Vulkan/Wayland 系统库；缺失的第三方库由 CMake 自动源码构建）
scripts/check-deps.sh

# 1) 生成测试媒体（10s mpeg4+mp2，需 ffmpeg CLI）
scripts/gen-test-media.sh test_media.mkv 10

# 2) 构建（debug 或 relwithdebinfo 两个预设任选）
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo

# 3) 播放（Vulkan 渲染窗口 + 声音；自动探测硬解，失败运行期回退软解）
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv --no-video   # 纯音频
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv --duration 5 # 限时

# 4) 转码/过滤（支持 rtsp:// http(s):// 输入）
./build/relwithdebinfo/kopaw/kopaw-transcode test_media.mkv out.mkv \
  --vc mpeg4 --ac aac --filter video:scale=320:180
```

播放器选项：`--backend vulkan|opengl` · `--width/--height` · `--no-audio` · `--no-video` ·
`--duration SEC` · `--filter video:<链>|audio:<链>` · `--zero-copy on|off|auto` ·
`--kopms-bus NAME`（视频零拷贝上屏）。
环境变量：`KOP_LOG=debug|info|warn|error` · `KOPAW_HWACCEL=auto|none|vaapi|cuda` ·
`KOPAW_ZERO_COPY=0`（强制关闭原生输出）· `KOPAW_STATS=1`（每 5s 性能基线）·
`KOPAW_SCHED=pool`（工作窃取池调度）。

## 用户级测试与 SDK（KOP_AppSDK_Interface）

面向应用/测试开发者的三层交付——SDK facade + 两个全流程测试应用 + 演示应用，
**退出码 = 失败项数**，可直接接入 CI：

```bash
./build/relwithdebinfo/apps/KOPAW_Test               # KOPAW 数据面全流程（无需窗口）
./build/relwithdebinfo/apps/KOPAW_Test --quick       # 快速模式
./build/relwithdebinfo/apps/KOPAW_Test --filter "scale=320:240"

./build/relwithdebinfo/apps/KOPMS_Test               # KOPMS 全流程（需显示环境）
./build/relwithdebinfo/apps/KOPMS_Test --skip-spawn --socket kop-0  # 接管已运行合成器

./build/relwithdebinfo/apps/KOP_Demo                 # SDK 演示：媒体→帧处理→零拷贝合成
./build/relwithdebinfo/apps/KOP_Demo --no-compositor # 纯数据面模式
```

- **KOPAW_Test**：ABI 契约 → 媒体探测 → 管线数据面（帧计数/分辨率/pts/状态收敛）→
  主动停止 → 错误路径 → lavfi 滤镜 → 性能基线。
- **KOPMS_Test**：能力探测 → 合成器生命周期 → BUS2LAYER 握手 → 控制面
  （窗口/焦点/剪贴板/ownership）→ 零拷贝发布全链 → 输入持久压测 → 优雅退出。
- **KOP_Demo**：SDK 活例子（约 150 行）——媒体 → 帧处理（缩放+动态色条）→
  零拷贝上屏 → 实时 FPS/发布/释放统计。实测 162 fps（零拷贝）/ 731 fps（纯数据面）。

SDK 核心 API（详见 [docs/sdk.md](docs/sdk.md) 与 `sdk/include/kop/app_sdk.hpp`）：

```cpp
kop::sdk::Pipeline         // 媒体 → 解码 → 帧回调（纯数据面，无窗口）
kop::sdk::FramePublisher   // CPU 帧 → Vulkan 导出 DMA-BUF → BUS 零拷贝上屏
kop::sdk::TestReport       // PASS/FAIL/SKIP 测试报告（退出码 = 失败数）
```

下游接入：`target_link_libraries(myapp PRIVATE kop::app-sdk)` +
`#include "kop/app_sdk.hpp"`。

## KOPMS 合成器

```bash
# nested 模式（窗口化输出，Wayland/X11 桌面下运行）+ 输入管线 + Vulkan 场景
./build/relwithdebinfo/kopms/kopms-compositor kop-0 60

# 另一个终端运行 Wayland 测试客户端（wl_shm 协议验证路径）
WAYLAND_DISPLAY=kop-0 ./build/relwithdebinfo/kopms/kopms-test-client 5

# 物理直出（logind 授权；kiosk/测试环境可加 --allow-unmanaged-drm）
./build/relwithdebinfo/kopms/kopms-compositor kop-0 60 --direct-drm
```

- BUS socket 自动发布为 `<wayland-socket>.bus`，KOPAW 可经 `--kopms-bus` 或
  SDK `FramePublisher` 零拷贝上屏；
- `--probe-drm` 只读枚举 DRM 资源；`--watch-drm` 监听热插拔；
- 输入：nested 窗口的真实鼠标/键盘经输入管线投递给 Wayland 客户端
  （wl_pointer/wl_keyboard，QWERTY evdev + xkbcommon keymap）。

## 测试体系

```bash
# 用户级全流程（推荐入口）
./build/relwithdebinfo/apps/KOPAW_Test && ./build/relwithdebinfo/apps/KOPMS_Test

# 引擎级
cd kopaw/core && cargo test                    # Rust 图引擎 8 项（所有权/停止/EOS/tee）
ctest --test-dir build/relwithdebinfo --output-on-failure   # 15 项 CTest
```

CTest 覆盖：lavfi 节点、插件 ABI（6 种异常 .so）、KOPMS GPU 模式/帧桥/DRM KMS/
DRM buffer/seat/热插拔/直出输出、BUS2LAYER 协议、**M3/M4 协议异常路径**
（malformed FD/重复 release/跨 session handle/fence 超时/异常退出/能力门控）、
**Vulkan DMA-BUF 跨设备像素回环**、**真实 Wayland dmabuf 客户端双轮验证**、
Wayland 协议 smoke、**输入/输出持久压测**（40 点击/104 键入 100% 投递 + fd 泄漏检查）。

零拷贝基准（提交→释放延迟/导入耗时/fence 等待/CPU 占用/回压丢失）：

```bash
./build/relwithdebinfo/kopms/kopms-dmabuf-bench      # JSON 输出
```

## 仓库结构

```
docs/                 architecture / kopaw-design / kopms-design / roadmap / sdk
common/               C++ 公共库（日志/时间/SPSC 环形缓冲/Vulkan 装置）
kopaw/
  abi/include/        kopaw_abi.h（cbindgen 生成，勿手改；当前 5.2）
  core/               Rust 图引擎（节点/链路/队列/时钟/调度/统计）
  modules/            C++ 节点（FFmpeg 解复用/解码/滤镜、PortAudio、渲染、
                      Vulkan DMA-BUF 导出器、KOPMS 零拷贝 sink）
  plugins/ tools/     demo 插件、kopaw-player、kopaw-transcode
  tests/              lavfi 节点与插件加载器测试
kopms/
  compositor/         Wayland 合成器（xdg/shm/dmabuf/explicit-sync/输入管线/场景）
  platform/           DRM/KMS 防护、Vulkan 场景（导入/合成/直出呈现）
  protocol/ bus2layer/  KOPMS-S/C BUS2LAYER（握手/双 lane/释放/控制面）
  clients/ tests/     测试客户端、输入反馈客户端、协议/回环/持久压测
sdk/                  KOP_AppSDK_Interface（include/ + src/）
apps/                 KOPAW_Test / KOPMS_Test / KOP_Demo
scripts/ cmake/       依赖体检、测试媒体生成、着色器嵌入
```

## 开发契约（红线，详见 docs/architecture.md）

1. **帧所有权**：引用计数 + 独占释放；`send` 返回 OK 即接管，禁止"先释放再返回非 OK"；
2. **ABI 头只由 cbindgen 生成**（当前 5.2），函数指针类型必须内联书写；
3. `dma_buf_handle` 不跨进程，跨进程一律走 FD 协议层（BUS2LAYER/SCM_RIGHTS）；
4. 图运行期不改图；EOS 经帧标志在数据面传递；汇聚节点显式 `kopaw_node_sink_done` 记账；
5. KOPMS 帧路径隔离：KOPAW 帧/dmabuf buffer 走 Vulkan 场景，绝不复用 CPU wl_shm 路径；
   场景只在 present/fence（直出为 page-flip）完成后释放帧；
6. **Wayland 请求处理器全覆盖**：NULL 处理器会让 libwayland 终止整个合成器进程。

## 许可证

本项目以 [GPL-3.0](LICENSE)（GNU General Public License v3）发布。
KOPAW/KOPMS 为可执行程序与动态节点组合，第三方插件通过稳定的 C ABI（`kopaw_plugin.h`）
接入，不构成派生作品的自动传染——插件自身的许可由其作者决定。

## 说明与限制

- FFmpeg v62 头文件无 extern "C"，C++ 包含需自行包裹；
- PortAudio/GLFW/glslang 系统缺失时由 CMake FetchContent 源码构建，首次配置需联网；
- VAAPI 可导出单对象双平面 NV12/P010 DMA-BUF；KOPAW Vulkan 后端以固定功能
  `VkSamplerYcbcrConversion` 导入，KOPMS 场景也可经 BUS 直通该类帧。VAAPI
  导出失败会回退系统内存，但本地 Vulkan 导入失败尚不能在运行中重新协商 CPU 路径；
- 当前帧/BUS 契约不携带色彩范围、矩阵或 HDR 元数据，有限范围 BT.601/709 由高度推断；
  CUVID 原生导出、full-range/BT.2020/HDR、dmabuf v4 feedback 和真机 KMS 直出验证属后续。
