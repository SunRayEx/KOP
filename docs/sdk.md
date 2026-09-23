# KOP App SDK —— 应用开发接口与用户级测试

`KOP_AppSDK_Interface` 把 KOPAW 数据面、KOPMS 合成器与 KOPNET 网络层的
内部细节封装成一组稳定的 C++ facade，应用/测试开发者无需接触节点 vtable、
图 ABI、BUS 协议或 socket/协议适配器即可开发与自测。`KOPAW_Test` 与
`KOPMS_Test` 是构建在这套 SDK 上的两个用户级全流程测试应用，
`kopnet-sdk-test` 是透明网络 facade 的覆盖测试（ctest 目标）。

## 构建与运行

```bash
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo

# KOPAW 数据面全流程测试（自动生成测试媒体；无需窗口/声卡）
./build/relwithdebinfo/apps/KOPAW_Test                # 标准模式
./build/relwithdebinfo/apps/KOPAW_Test --quick        # 快速模式
./build/relwithdebinfo/apps/KOPAW_Test --media xx.mkv --filter "scale=320:240" --audio

# KOPMS 合成器全流程测试（需要显示环境；自动拉起合成器）
./build/relwithdebinfo/apps/KOPMS_Test                # 全流程（协议/控制面/零拷贝/输入soak）
./build/relwithdebinfo/apps/KOPMS_Test --frames 60 --input-rounds 8
./build/relwithdebinfo/apps/KOPMS_Test --skip-spawn --socket kop-0   # 接管已运行合成器

# KOPNET 透明网络 facade 测试（ctest 目标；无需显示环境）
cmake --build --preset relwithdebinfo --target kopnet-sdk-test
./build/relwithdebinfo/sdk/kopnet-sdk-test
```

两个应用输出 `[PASS]/[FAIL]/[SKIP]` 明细与汇总，**退出码 = 失败项数**，
可直接接入 CI 或脚本。

## KOPAW_Test 覆盖流程

1. ABI 契约校验（SDK ↔ 宿主引擎版本/能力位）
2. 媒体探测（缺失时自动调用 `scripts/gen-test-media.sh`）
3. 管线数据面：解复用 → 解码（硬解探测/回退）→ 帧回调；分辨率/步长/数据合法、
   状态收敛（FINISHED）、性能基线 JSON
4. 主动停止路径（stop → STOPPING 收敛，不挂死）
5. 错误路径（不存在媒体 → 干净失败并给出原因）
6. lavfi 滤镜链（`--filter`，验证分辨率生效）

## KOPMS_Test 覆盖流程

1. Vulkan DMA-BUF 导出能力探测
2. 合成器生命周期（fork + socket 就绪 + SIGTERM 优雅退出）
3. BUS2LAYER 协议握手（Handle/modifier/explicit-sync 能力协商）
4. 控制面（窗口创建/绑定/焦点/剪贴板/ownership，ACK 校验）
5. 零拷贝发布：CPU 帧 → Vulkan 导出 → BUS 提交 → FRAME_RELEASE 全链
   （发布数/失败数/回收数/在飞清空/耗时）
6. 输入/输出持久压测（委托 `kopms-input-soak-test`：鼠标点击/键入字符的
   注入-反馈回路 + fd 泄漏检查）

## SDK API 速览

```cpp
#include "kop/app_sdk.hpp"

// ---- KOPAW：媒体管线（数据面，无窗口）----
kop::sdk::PipelineOptions opts;
opts.media = "test_media.mkv";
opts.on_frame = [](const kop::sdk::FrameView& f) {
    // f.width/f.height/f.stride/f.data/f.pts —— 回调返回前有效
};
std::string err;
auto pipeline = kop::sdk::Pipeline::open(opts, &err);
pipeline->start(&err);
pipeline->wait_finished(30000);
std::string stats = pipeline->stats_json();
pipeline->stop(2000);

// ---- KOPMS：零拷贝发布（单线程使用）----
kop::sdk::FramePublisher pub;
kop::sdk::PublisherOptions popts;
popts.bus_socket = "kop-0.bus";
popts.window_id = 42;
pub.connect(popts, &err);
pub.publish_cpu_frame(rgba, w, h, w * 4, pts_us, &err);  // 内部完成 Vulkan 导出
pub.poll(10);                                            // 泵 FRAME_RELEASE
pub.focus_window(42, &err);
pub.disconnect();

// ---- KOPNET：透明网络（逻辑通道，URI 驱动）----
kop::sdk::NetTunnelOptions nopts;
nopts.uri = "tcp://127.0.0.1:19700";          // 也支持 unix:// rtp:// udp:// ssh:// …
nopts.serve = false;                            // true = 服务端（listen+accept）
nopts.auto_reconnect = true;                    // 拨号端断线指数退避自动重连
nopts.on_data = [](const std::string& kind, const uint8_t* data, size_t len) {
    // kind 命中的逻辑通道收到 len 字节负载（工作线程）
};
nopts.on_state = [](bool connected) {           // 连通状态变化
};
auto link = kop::sdk::NetTunnel::open(nopts, &err);
link->wait_connected(5000);                     // 首连屏障
const uint32_t ch = link->open_channel("media", /*ordered=*/false, &err);
link->send("media", frame.data(), frame.size(), &err);
link->send(ch, frame.data(), frame.size(), &err);   // 也可直接用通道号
link->close_channel("media");
link->close();                                  // 幂等；析构也会调用

// ---- 测试报告 ----
kop::sdk::TestReport report("MyTest");
report.check(cond, "用例名", "详情");
report.summary();
return report.failures();
```

## KOPNET 透明网络（NetTunnel）

`NetTunnel` 是 KOPNET 的应用层 facade：应用只面对**逻辑通道**（kind 名），
不接触 socket、协议适配器、分帧与重连细节。一个 URI 决定传输与协议，
`serve` 决定角色。

### 选项（NetTunnelOptions）

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `uri` | — | KOPNET URI：`tcp://` `unix://` `udp://` `rtp://` `pipe://` `stdio://` `ssh://` 等 |
| `serve` | `false` | `true`=服务端（listen + accept），`false`=拨号端 |
| `on_data` | — | 收到数据：`(kind, data, len)`，**工作线程** |
| `on_state` | — | 连通状态变化：`(connected)`，**工作线程** |
| `on_channel` | — | 对端打开通道：`(kind)` |
| `on_log` | 走 `KOP_LOG` | 日志回调（stderr 之外的自定义收集） |
| `auto_reconnect` | `true` | 拨号端断线自动重连（指数退避，逻辑通道号跨重连不变） |
| `reconnect_base_ms` | 500 | 首次退避基数 |
| `reconnect_max_ms` | 5000 | 退避上限 |
| `handshake_ms` | 5000 | 隧道握手与通道打开超时 |

### 契约

- **只搬字节**：`send` 接受连续内存，`on_data` 投递负载。**fd / DMA-BUF 零拷贝
  不在本 facade**——需要透传句柄请用 net 节点（`net_source` / `net_sink`，
  AF_UNIX 上的 SCM_RIGHTS），本 facade 负责为节点提供传输。
- **角色硬隔离**：服务端只能**接受**对端打开的通道，`open_channel()` 返回错误；
  拨号端可主动开通道。
- **服务端是“单当前对端”模型**：新连接接入会替换上一条会话，旧通道映射清空。
- **kind 在本端唯一**：同名后开覆盖先开的映射。
- `ordered=true` → Stream（流通道，分帧有序）；`false` → Datagram（数据报）。
  Stream 通道不能开在数据报传输（`rtp://` / `udp://`）上。
- **回调都在工作线程**：不要在回调里调用 `close()`；需要业务线程消费请把数据拷走。
- `close()` 幂等且线程安全，析构会调用；调用后不要再使用其它方法。
- `wait_connected()` 是首连屏障；之后可用 `connected()` / `stats()` 观测状态。

### kopnet-sdk-test 覆盖流程

1. `tcp://` 回显：拨号 + 服务端双向通信、payload 完整、`on_channel`、`stats`/`stats_json`
2. 多通道按 kind 路由（Stream `control` + Datagram `media` 共存）
3. 服务端开通道被拒（明确错误文本）
4. `unix://` 自动重连：服务端销毁 → 拨号端感知断开 → 同址重建 →
   同一逻辑通道恢复通信
5. 关闭自动重连：断线即终止，发送得到明确错误
6. `close_channel()` 后发送得到“未打开”错误

> unix 服务端销毁时监听器会 unlink socket 文件；同址重建服务端时必须
> **先彻底销毁旧实例再 bind**，否则旧实例的析构会删掉新实例的文件
> （测试第 4 项即验证此顺序）。

## 契约与边界

- **帧所有权**：SDK 回调期间帧归管线（引用计数 + 独占释放）；需留存请拷贝。
- **句柄不跨进程**：`dma_buf_handle` 只在本进程可解释；跨进程一律走
  BUS2LAYER 的 FD 传递（SDK 的 `FramePublisher` 已封装）。
- **线程模型**：`Pipeline` 在内部节点线程回调 `on_frame`；`FramePublisher`
  的全部方法必须单线程调用。
- **窗口呈现**：`Pipeline` 是纯数据面（不建窗口）；需要本地窗口请用
  kopaw-player 或自建渲染节点。KOPMS 侧的画面呈现由合成器完成。
- **网络字节边界**：`NetTunnel` 只把字节负载按逻辑通道送达，不解释负载格式
  （不组帧/不转码）；需要 fd 透传走 net 节点的 SCM_RIGHTS。

## 演示应用（KOP_Demo）

`sdk` 的活例子——用约 150 行用户代码串起全部三层能力，开发者可直接对照源码
（`apps/kop_demo.cpp`）学习 SDK 用法：

```bash
./build/relwithdebinfo/apps/KOP_Demo                  # 完整链路：媒体 → 帧处理 → KOPMS 零拷贝合成
./build/relwithdebinfo/apps/KOP_Demo --no-compositor  # 纯数据面模式（无合成器/显示也能跑）
./build/relwithdebinfo/apps/KOP_Demo --effect none --seconds 5
```

演示流程：`Pipeline::open` 打开媒体 → 回调内做帧处理（最近邻缩放 + 动态色条 +
帧号）→ `FramePublisher::publish_cpu_frame` 零拷贝发布 → 合成器场景合成 →
周期打印 FPS/发布/释放/在飞 → 媒体自然结束或时长上限后优雅收尾。
实测（MX450 + llvmpipe，320x240）：零拷贝模式 162 fps（900/900 发布/释放），
纯数据面 731 fps。

## 下游接入方式

- 同仓构建树：`target_link_libraries(myapp PRIVATE kop::app-sdk)` +
  `#include "kop/app_sdk.hpp"`（apps/ 下的两个测试应用即参考实现）。
- 安装交付：`sdk/include/` 头文件 + `libKOP_AppSDK_Interface.a` +
  传递依赖（kopaw-modules / kopaw_core / kopms-protocol / kopnet），
  随构建树一起分发。
