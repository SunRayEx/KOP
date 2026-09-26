# KOP 总体架构

> KOP（Kongar Pipe）：整合音视频管线（对标 PipeWire）与显示服务器（对标/替代 Xorg）的系统项目。
> 核心语言 C/C++/Rust 按模块混编；图形后端 Vulkan/OpenGL 编译期可选；音频后端 PortAudio。

## 两大主体

| 子系统 | 名称 | 定位 | 优先级 | 状态 |
|---|---|---|---|---|
| 音视频管线 | **KOPAW**（Kongar Pipe Audio/Video Wire） | 媒体图引擎 + 节点库，对标 PipeWire 的管线能力 | **第一优先级** | MVP 已实现（CLI 播放器纵向切片） |
| 显示服务器 | **KOPMS**（Kongar Pipe Monitor Server） | Wayland 兼容合成器，替代 Xorg | 第二阶段 | P3-M1 协议骨架完成；M2 平台防护骨架 |
| 远程控制支撑 | **KOPNET**（Kongar Pipe Network） | 透明网络层：逻辑 Channel 多路复用 / 回压 / fd 透传 | 支撑层 | 三层模型 + relay 已落地（见 docs/kopnet-design.md） |

## 语言分工（已决策：按模块混合）

```
┌────────────────────────────────────────────────────────────┐
│                       应用层（C++）                          │
│            kopaw-player CLI / 未来的 KOPMS 宿主              │
├────────────────────────────────────────────────────────────┤
│   C ABI 边界（kopaw_abi.h，cbindgen 生成，唯一定义源）        │
├────────────────────────────┬───────────────────────────────┤
│  Rust：kopaw-core 图引擎    │  C++：kopaw-modules 节点库      │
│  · 节点/链路/端口            │  · FFmpeg demux/decode/resample │
│  · 有界队列（背压）          │  · PortAudio 音频汇聚           │
│  · 媒体时钟（A/V 同步）      │  · Vulkan/OpenGL 渲染后端       │
│  · 每节点一线程调度          │  · 帧所有权助手（OwnedFrame）    │
├────────────────────────────┴───────────────────────────────┤
│  系统库：FFmpeg 8 / Vulkan 1.3 / PortAudio(ALSA) / GLFW      │
└────────────────────────────────────────────────────────────┘
```

分工原则：
- **Rust** 承担并发调度、队列、时钟等安全关键逻辑（内存安全 + 现代工具链）；
- **C++** 承担与 C 库（FFmpeg/PortAudio/Vulkan）紧贴的节点实现（零 FFI 摩擦）；
- **C ABI** 是二者唯一边界：类型 `#[repr(C)]` 定义在 Rust 侧，`cbindgen` 生成
  `kopaw/abi/include/kopaw_abi.h` 供 C++ 包含；C++ 侧不引入任何 Rust 头。

## 构建体系

- 顶层 **CMake 超构建**（`CMakePresets.json` 提供 debug/release/relwithdebinfo）；
- Rust crate 经 **Corrosion** 集成（FetchContent 拉取），产物 `libkopaw_core.a` 链入 C++；
- 系统缺失的依赖（GLFW 3.4、PortAudio 19.7、glslang 15）一律 **FetchContent 源码构建**，
  项目自包含；FFmpeg/Vulkan 用系统库（一律走 pkg-config，Fedora 头文件在 `/usr/include/ffmpeg`）；
- 着色器（GLSL → SPIR-V）由构建期 glslang 编译后**嵌入二进制**（`cmake/EmbedShaders.cmake`），
  无运行时文件依赖。

```bash
scripts/check-deps.sh                  # 依赖体检
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
ctest --test-dir build/relwithdebinfo  # 全量回归（27 项）
cargo test                             # kopaw-core 单元测试（在 kopaw/core 下）
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv
```

### FFmpeg 封装层（`kopaw/modules/ffmpeg/`）

KOPAW 的所有 libav* 调用都经由四个头文件收口，保证“一套包含入口 + 一套错误映射
+ 一套 RAII + 一套版本垫片”：

| 文件 | 职责 |
|---|---|
| `ffmpeg.hpp` | 唯一的 umbrella 包含文件：所有 libav* 头在**一个** `extern "C"` 块里（FFmpeg 8 头不再自带保护），并声明 `__STDC_CONSTANT_MACROS` 等宏 |
| `ffmpeg_error.hpp` | `av_error_string()` / `av_status()` / `av_is_retry()`：把 FFmpeg 返回码统一映射到 `KOPAW_E_*`（EAGAIN/EOF 一眼可辨） |
| `ffmpeg_raii.hpp` | `AvPtr<T, Deleter>` + `AvFrame`/`AvPacket`/`AvCodecContext`/`AvFormatContext`/`AvFilterGraph`/`AvBufferRef`/`AvSws`/`AvSwr`/`AvAudioFifo`/`AvDictionary`：替换全部手工 free/unref |
| `ffmpeg_compat.hpp` | 版本垫片：声道布局 API（libavutil ≥57.28 新旧写法）与编解码器能力查询（FFmpeg 8 `avcodec_get_supported_config` vs 已弃用的 `AVCodec::pix_fmts` 等） |

模块内**不再直接 `#include <libav*/...>`**，也不再手写 `extern "C"`；新增出口的
“漏释放”被 RAII 从结构上消除。`AvFormatContext` 等所有权经 `avformat_open_input`
这类“接管又可能归还”的 API 时，用 `release()` / `reset()` 显式交出/取回裸指针。

### 模糊测试（`fuzz/`，`KOP_BUILD_FUZZERS=ON`）

纯解析器是“把不可信字节变成内存访问”的地方，也是唯一值得长年模糊的层。当前三个靶：

| 靶 | 被测代码 |
|---|---|
| `kopnet-rtp-fuzz` | RTP/RTCP/STUN 头部解析（`parse_rtp`/`parse_rtcp`/`parse_stun`/`classify_media_packet`） |
| `kopnet-tunnel-fuzz` | 隧道线协议（`decode_tunnel_header`/`decode_control`） |
| `kopnet-frame-envelope-fuzz` | 帧信封反序列化（`deserialize_frame`：平面计数/stride/载荷偏移全部来自 wire） |

harness 只实现 `LLVMFuzzerTestOneInput`，运行引擎由构建系统自动选择：
clang 可用时是**真 libFuzzer**（`-fsanitize=fuzzer,address,undefined`），否则回退到
**standalone 驱动**（`fuzz_driver.cpp`：xorshift 变异 + ASan 死亡回调落盘 `crash-*.bin`）。

```bash
scripts/run-fuzzers.sh                       # 本地复现 CI 冲烟（默认 20 万次/靶）
scripts/run-fuzzers.sh 2000000               # 加深预算
KOP_FUZZ_ENGINE=standalone scripts/run-fuzzers.sh   # 无 libFuzzer 时
```

种子语料由各 harness 的 `kop_fuzz_make_seeds()` 提供“结构上有效”的报文——
纯随机字节只会反复命中“过短/魔数错误”的早返回，进不了解析器内部路径。

## 关键约定（跨模块红线）

1. **帧所有权：引用计数 + 独占释放**：`KopawFrame` 由生产者分配（refs=1）；
   队列/引擎只搬运指针，不解引用数据；消费者用完必须 `release(frame)`；
   多出边（tee）投递时引擎对后续边调用 `retain(frame)`，最后一个引用真正释放。
   节点 `send` 返回 OK 表示接管帧（此后由节点释放），返回非 OK 表示**未接管**
   （引擎负责释放）——**禁止"先释放再返回非 OK"**。
2. **ABI 头文件只由 cbindgen 生成**（`kopaw/core/cbindgen.toml`），禁止手改；
   重新生成命令见 toml 头部注释。函数指针类型必须内联书写（cbindgen 限制）。
   当前 ABI 为 5.2（在 Vulkan external memory 契约之上新增
   `KOPAW_CAP_MULTI_INPUT`/`send_port` 和多平面 DMA-BUF 的 `drm_fourcc`）；
   媒体内存统一由 `uint64_t dma_buf_handle` 标识，插件还需协商次版本和能力位。
   CPU 帧的句柄只允许在当前进程内解码为地址别名，DMA-BUF/Vulkan external memory
   跨进程必须由协议层传递 FD。原生 YUV 帧还必须给出 fourcc、每平面的
   fd/offset/stride/modifier，消费者不得从尺寸或内存类型猜测格式。插件输出节点的
   `bind_output` 在图注册后接收真实端口句柄，Rust 引擎与 C++ 节点基于同一契约构建。
3. **FFmpeg 8 头文件不再自带 extern "C"**：C++ 包含时必须自行包裹；
   `kopaw_abi.h`/portaudio.h/GLFW 自带保护，**绝不能再包一层**。
4. 图运行期（start 之后）不允许改图；EOS 经帧标志（`KOPAW_FRAME_FLAG_EOS`）在
   数据面传递，保证与数据严格有序。
5. **汇聚节点完成 = 显式记账**：引擎不在 EOS 交付时自动计数；需要尾后处理
   （音频 ring 排空）或绘制收尾的汇聚节点，在真正完成时调用
   `kopaw_node_sink_done(g, node)`；全部汇聚节点记账完毕才触发 FINISHED。
6. **KOPMS 帧路径隔离**：M1 的 CPU `wl_shm` buffer 只服务协议验证；KOPAW 帧与
   Wayland dmabuf buffer 必须通过 `KopmsFrameDescriptor` / `DmabufBufferView` 传递
   `dma_buf_handle`、DMA-BUF 平面、modifier、同步 fence 和 release 回调，统一进入
   Vulkan 场景（`vk_scene`），合成器不得把 GPU 路径与 wl_shm 路径混为同一所有权
   模型。`SCM_RIGHTS` 接收后只能使用服务端本地 FD，不能解读客户端的数字 FD。
   场景只在 present/render-fence（DRM 直出为 page-flip）完成后释放帧。
7. **KOPMS BUS2LAYER 分流**：窗口树、焦点、剪贴板和 ownership 走 CONTROL lane，
   KOPAW native 帧走 MEDIA lane；两者共享版本/能力协商和 session，但 payload 与
   FD 所有权语义分别校验。Handle（KOPAW_MEMORY_VULKAN）、modifier 和 explicit-sync
   （acquire fence）必须经 HELLO 能力位协商后使用。
8. **Wayland 请求处理器全覆盖**：合成器发布的每个接口，其版本范围内所有可达
   请求都必须有实现函数（不支持的语义为显式 no-op 并注明）。NULL 处理器会让
   libwayland-server `wl_abort` 杀死整个合成器进程（实测 GTK3 的 set_parent/
   region.add 即触发）。新增全局对象时必须同步核对生成头文件的接口成员表。
9. **KOPNET 队列与 fd 所有权**（见 docs/kopnet-design.md）：
   `BoundedPacketQueue::put(Packet&&)` 仅在返回 Ok 时接管包（含其 fd），
   Timeout/Closed 时调用方保留所有权——重试循环里重复传同一个包必须安全；
   隧道发送侧在帧真正写出（或失败）后必须关闭帧携带的 fd，停机时滞留帧同样回收；
   传输层 fd 透传由 `supports_fds()` 显式声明，DMA-BUF fd 必须先 `dup` 再入队
   （帧 release 与异步发送的时序解耦）。
