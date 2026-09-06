# KOP 总体架构

> KOP（Kongar Pipe）：整合音视频管线（对标 PipeWire）与显示服务器（对标/替代 Xorg）的系统项目。
> 核心语言 C/C++/Rust 按模块混编；图形后端 Vulkan/OpenGL 编译期可选；音频后端 PortAudio。

## 两大主体

| 子系统 | 名称 | 定位 | 优先级 | 状态 |
|---|---|---|---|---|
| 音视频管线 | **KOPAW**（Kongar Pipe Audio/Video Wire） | 媒体图引擎 + 节点库，对标 PipeWire 的管线能力 | **第一优先级** | MVP 已实现（CLI 播放器纵向切片） |
| 显示服务器 | **KOPMS**（Kongar Pipe Monitor Server） | Wayland 兼容合成器，替代 Xorg | 第二阶段 | P3-M1 协议骨架完成；M2 平台防护骨架 |

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
cargo test                             # kopaw-core 单元测试（在 kopaw/core 下）
./build/relwithdebinfo/kopaw/kopaw-player test_media.mkv
```

## 关键约定（跨模块红线）

1. **帧所有权：引用计数 + 独占释放**：`KopawFrame` 由生产者分配（refs=1）；
   队列/引擎只搬运指针，不解引用数据；消费者用完必须 `release(frame)`；
   多出边（tee）投递时引擎对后续边调用 `retain(frame)`，最后一个引用真正释放。
   节点 `send` 返回 OK 表示接管帧（此后由节点释放），返回非 OK 表示**未接管**
   （引擎负责释放）——**禁止"先释放再返回非 OK"**。
2. **ABI 头文件只由 cbindgen 生成**（`kopaw/core/cbindgen.toml`），禁止手改；
   重新生成命令见 toml 头部注释。函数指针类型必须内联书写（cbindgen 限制）。
   当前 ABI 为 5.1（新增 KOPAW_MEMORY_VULKAN 与 KOPAW_CAP_VULKAN_EXTERNAL）；媒体内存
   统一由 `uint64_t dma_buf_handle` 标识，插件还需协商次版本和能力位。CPU 帧的句柄
   只允许在当前进程内解码为地址别名，DMA-BUF/Vulkan external memory 跨进程必须由
   协议层传递 FD。插件输出节点的 `bind_output` 在图注册后接收真实端口句柄，
   Rust 引擎与 C++ 节点基于同一契约构建。
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
