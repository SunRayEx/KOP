# KOPNET 设计：透明远程控制支撑层

> KOPNET 是 KOP 的远程控制支撑层。目标：**应用层只面对逻辑 Channel，不接触
> socket、协议或序列化细节**——无论对端在本机、SSH 对端、RTC 会话还是 RDP 通道，
> KOPAW/KOPMS 的代码路径都完全一样。

## 三层模型

```
┌──────────────────────────────────────────────────────────┐
│  应用层                                                    │
│  · KOPAW 节点（net_source / net_sink）                    │
│  · KOPMS remote_session                                    │
│  · kopnet-relay CLI                                       │
├──────────────────────────────────────────────────────────┤
│  Tunnel（kopnet/tunnel）                                   │
│  · 逻辑 Channel 多路复用（kind 区分流，id 奇偶分配）        │
│  · 20B 定长帧头 + payload（流式精确读取状态机）             │
│  · 控制消息：HELLO / OPEN / OPEN_ACK / CLOSE / CREDIT      │
│  · credit 有界队列回压（端到端可阻塞）                     │
│  · SCM_RIGHTS fd 透传（对齐 BUS2LAYER 语义）               │
├──────────────────────────────────────────────────────────┤
│  ProtocolAdapter（kopnet/protocol）                       │
│  · scheme → Transport 的适配注册表                         │
│  · tcp / udp / unix / relay 直连；ssh spawn 子进程         │
│  · rtc / rtp / rdp：各自协议的一条连接包装成 Transport     │
├──────────────────────────────────────────────────────────┤
│  Transport（kopnet/transport）                             │
│  · 流式（read/write）与数据报（send/recv_datagram）双语义   │
│  · 非阻塞 + poll；fd 透传能力由 supports_fds() 声明        │
└──────────────────────────────────────────────────────────┘
```

设计原则与 KOPAW 一致：**有界队列即背压**。应用线程只往
`channel.send_queue`（有界）里放包，工作线程单线程完成读分帧、
控制协议、credit 记账与写复用，因此应用线程的发送阻塞会自然地
反压到对端接收方，整条链路端到端可阻塞。

## 端点与传输

`Endpoint`（`kopnet/endpoint`）解析 URI：`scheme://[user@]host[:port]/path`。

| scheme | Transport | 备注 |
|---|---|---|
| `tcp://h:p` | TCP 流 | 默认；`supports_fds()`=false |
| `udp://h:p` | UDP 数据报 | serve 端按首包 connect |
| `unix://path` | AF_UNIX 流 | `supports_fds()`=true，可透传 DMA-BUF |
| `relay://name` | AF_UNIX `$XDG_RUNTIME_DIR/kopnet/relay/<name>` | 本机命名中继 |
| `ssh://h/cmd` | 子进程 stdio 管道 | spawn `ssh -T -o BatchMode=yes …` |
| `rtp://h:p` | RTP over UDP | 会话化 UDP + RTP 头封装/解封装，见下节 |
| `rtc://` / `rdp://` | 未实现 | 需 DTLS-SRTP/ICE 栈，已注册但返回“尚未实现” |

`Transport` 是“一条已经建好的数据通路”：Tunnel 只面向 `Transport` 编程，
因此对协议完全透明。复杂 scheme（ssh/rtp/rtc/rdp）由 ProtocolAdapter 产生
Transport，不经 `open_raw_transport`（后者只认 tcp/udp/unix）。

写半连接 socket 或已关闭读端的管道会触发 SIGPIPE，其默认处置是杀死进程——
在断线重连场景里这是必发的（对端刚断开，本端还在写）。故 socket 路径的
所有发送一律带 `MSG_NOSIGNAL`，并且库入口（`open_raw_transport` /
`create_transport_listener` / `spawn_pipe_process`）统一忽略 SIGPIPE：写错误
只经 `IoStatus::Error` + errno 上报。

### RTP 适配器（`rtp://`）

`rtp://host:port` 在一条 UDP 数据报通路上叠加 RFC 3550 报文分帧，语义仍是
`Datagram`。URI 查询参数映射到 `RtpConfig`：`pt`（负载类型，默认 96）、
`clock`（时钟频率，默认 90000）、`ts_step`（每包时间戳增量，默认 0 表示按
`fps` 推导）、`fps`（默认 30）、`ssrc`（默认 0 表示随机）。

- 发送：应用数据报 → 前置 12 字节 RTP 头（V=2，PT/SSRC 取配置，序号自增、
  时间戳按步进）→ UDP。序号与时间戳初值随机（RFC 3550 建议）。
- 接收：解析头并校验版本/负载类型，剥除 CSRC 列表、头部扩展与 padding 后
  投递负载；16bit 回绕序号统计丢包与重复（`RtpStats`），不重排、不缓存，
  乱序包按到达顺序投递。
- 复用：同一五元组上的 RTCP（PT 192–223）/STUN 报文被识别后跳过并计入
  stats，`recv_datagram` 对它们返回 `WouldBlock`（调用方继续等读）。
- 监听端复用 UDP 的会话化逻辑：bind 后收首包、connect 锁定对端，首包作为
  pending 交给 `RtpTransport`（不丢失）。

**限制**（有意为之，职责划在上层）：不做分片/重组，负载超过数据报上限直接
`Error`（>1400 字节告警提醒 MTU）；不生成 RTCP（拥塞控制与带宽估计不在
KOPNET 范围）；不支持 fd 透传（UDP 无 SCM_RIGHTS 语义）；不做 SRTP 加密。

## 隧道协议

### 帧（小端）

```
magic(4) "KPNT" | version(2) | flags(2) | channel_id(4)
payload_len(4)  | reserved(4)
payload（payload_len 字节）
```

- 流式传输上，一帧可能跨多次 `read`；状态机按 `payload_len` 精确读取到帧边界
  才投递，**fd 归属因此永远明确**（SCM_RIGHTS 只能随某一次 recvmsg 到达，
  部分写时 fd 只随首片）。
- 数据报传输上一帧一个 datagram；Stream 通道落在数据报传输上时适配器层直接拒绝。

### 控制消息

`payload = op(4) + body`：`HELLO`（版本/能力位/max_channels/credit/max_frame）、
`OPEN`/`OPEN_ACK`（channel_id/mode/kind）、`CLOSE`、`CREDIT`（grant 额度）。

- 通道 id：**dialer 分配奇数、listener 分配偶数**，双侧可同时开通道而不冲突。
- HELLO 能力位声明是否支持 fd 透传、Stream/Datagram 通道。
- **握手不阻塞 start()**：两端无法同时阻塞在 start（顺序启动时先启动的一端必然
  超时）。`start()` 只启动工作线程立即返回；`open_channel` 按需 `wait_hello()`，
  工作线程自带 HELLO 超时检测（`last_error_="HELLO 协商超时"`）。
- **接受侧通道立即可用**：OPEN 分派时即 `open_ok=true`（无需等 OPEN_ACK）；
  打开侧在 OPEN_ACK 成功后才可用，并同时授予初始 credit。双向初始 credit
  对称授予（OPEN 接受方授予 + OPEN_ACK 打开方授予）。

### 所有权红线

- `BoundedPacketQueue::put(Packet&&)`：**仅 Ok 时接管 Packet（含其 fd）**；
  Timeout/Closed 时调用方保留所有权。重试循环里重复传同一个包是安全的
  （早期按值接收的实现会在队列满后把"已被移空的壳"入队，产生幽灵空包并丢失 fd——
  现由 `kopnet-queue-test` 锁定）。
- 发送侧：帧完整写出（或失败）后必须关闭帧携带的 fd（成功时 fd 已随
  SCM_RIGHTS 移交对端内核，这里关的是本进程引用副本）；停机时滞留
  `pending_writes_` 的帧也由本端回收。
- 接收侧：收到的 fd 由应用拥有；`NetSourceNode` 当前只回放 CPU 载荷，直接关闭。
- `NetSinkNode` 对 DMA-BUF 必须 `dup`：帧 release 会关闭原 fd，而隧道发送是异步的。
- 传输不支持 fd 透传时，DMA-BUF 零拷贝路径直接丢弃并回收 dup（记 `frames_dropped_`）。

### 回压

- 每个 Channel 有一对有界队列（send/recv），容量 = credit。
- 消费到低水位（credit/2）时补授权 CREDIT，避免逐包往返。
- 有界队列天然背压，与 KOPAW 的图队列一致。
- **socket 缓冲打满不是错误**：`flush_writes()` 在 `WouldBlock` 时把帧放回
  `pending_writes_` 队首并返回 true，工作循环随后 `wait_writable` 再重试；
  只有真正的 I/O Error 才终止会话（早期实现把 WouldBlock 当致命错误，
  生产速率超过 socket 排空速率时会误杀隧道——现由
  `test_socket_backpressure` 锁定：64×60KB 洪泛 + 慢消费，全部严格有序）。
- `TunnelSession::stop()` 幂等且可重入：以 `std::once_flag` 保证只真正停机
  一次（停标志 → join 工作线程 → 关闭全部通道）。上层（如 KOPMS
  remote_session 的 `disconnect()` 与析构）可能在两条路径上各调一次，甚至
  与析构并发调用，重入的 `join` 会是未定义行为。

## 节点（KOPAW 集成）

- `NetSourceNode`：自驱动源节点。`open()` 时拨隧道并开通道；`run()` 循环
  `recv`，按帧信封（见下）重建一帧并 `kopaw_graph_emit`；通道关闭或空闲超时
  退出并下发 EOS。
- `NetSinkNode`：响应式汇聚节点。`send_impl` 把帧按帧信封序列化后投递到隧道，
  平面 fd 与 acquire fence fd 随该消息经 SCM_RIGHTS 透传（`dup` 后发送，
  帧释放与发送时序解耦）；EOS 帧调用 `kopaw_node_sink_done` 记账。
- **会话韧性 opt-in**：两个节点的 Options 都带 `auto_reconnect` 与
  `fallback_endpoints`。开启后通道走 `ResilientSession`（见下）：传输断开时
  自动重拨/故障转移，逻辑通道号不变——`NetSourceNode` 的 `recv` 自动挂起
  等待重连，`NetSinkNode` 的发送在断开期间按丢帧处理（回压语义显式化）。
  节点内部经 `NetChannel` 接口统一裸会话与重连会话，节点逻辑不感知差别。
- **输出句柄由应用显式回填**：引擎当前不回调 `bind_output`，应用在
  `kopaw_graph_add_node` 后必须调用 `node->set_output(kopaw_graph_node_output(g, id, 0))`
  （与 `DemuxerNode::set_outputs` 同一约定）。

### 帧信封（frame_envelope）

`KopawFrame` 在 `media` 通道上的线上格式。一条消息 = 一个定长小端头（132B：
magic `KOPF` / 版本 / 内存类型 / 帧标志 / pts / dts / 宽高 / drm_fourcc /
stride / plane_count / fence_kind / fd 计数与位图 / payload 长度 / 色彩与 HDR
元数据）+ 每平面 16B 记录（offset / stride / modifier）+ 载荷（仅 CPU 帧）。
随消息到达的 fd 顺序固定：先 plane fd（按 plane 序，跳过 fd<0），最后 fence fd；
反序列化时按位图把 fd 逐一贴回对应平面，**fd 与平面对齐由测试
`fd_bytes_equal` 逐平面 mmap 比对锁定**。同一 DRM 对象的多平面帧会收到多个 fd
（各指向同一对象），由帧 `release` 的 `close_external_fds` 逐个关闭。

## 中继（kopnet-relay）

```
kopnet-relay --listen tcp://0.0.0.0:19400 --target unix:///run/kopnet/relay/hub
kopnet-relay --stdio --target tcp://hub:19400     # SSH 远端伙伴
ssh host kopnet-relay --stdio --target tcp://hub:19400
```

接入侧每开一个通道，就在目标侧开同 kind 通道并双向泵。stdio 模式把
`stdin/stdout` 接成 Transport（`make_stdio_transport`），配合 SSH 适配器即可
获得加密的远程控制通道——无需在进程内嵌入 SSH 加密栈（与 git 的 ext:: /
mosh 的 SSH 引导同一路数）。

## 会话韧性（kopnet/resilient）

`TunnelSession` 绑死在单条 Transport 上：传输一断（WiFi 抖动、SSH 被杀、
中继重启）会话即死。`ResilientSession` 在其之上加一个连接线程，提供**断线
自动重连 + Endpoint 列表故障转移**：

  * 应用只面对**逻辑通道号**（`open_channel` 返回，跨重连不变）；连接线程
    在每次握手成功后把已注册通道全部重开，新的线上通道号经映射表更新。
    `set_channel_handler` 在首开与每次重连后回调，应用据此重装数据处理器
    或重放状态。
  * **退避重拨**：`base_delay_ms` 起步、倍增至上限 `max_delay_ms`；每次重连
    都从 Endpoint 列表头开始拨，主路径恢复后自动切回。`max_attempts` 耗尽后
    停止（`reconnect_done()` 为真），默认无限重试。
  * 断开期间 `send()` 立即返回 `Closed`（不阻塞、不丢线程），`recv()` 停在
    条件变量上等重连，恢复后从新通道继续收——总等待不超过调用方给的超时。
    `set_state_handler` 通知断开/恢复，供上层（如 KOPMS remote_session）把在途
    帧标记为丢弃。

线程安全要点：会话用 `shared_ptr` 持有，发送/接收路径拷一份副本后**锁外**
调用，析构（`~TunnelSession` → `stop` → join 隧道工作线程，其退出时会回调
`close_handler`）绝不发生在持锁区；过期的通道号用 `epoch_` 计数器丢弃
（应用线程的立即开通道与连接线程的重开可能交错）。

## KOPMS 远程会话（kopnet/remote）

`RemoteSession` 把 KOPMS 线协议（32B 定长头 + payload）装在两条隧道通道上：
`kopms.control`（控制 lane）与 `kopms.media`（媒体 lane）。connect 侧 `open()`
直拨并完成 HELLO 能力协商，serve 侧接受连接后 `serve()` 装回调再 `start()`
隧道——通道回调在首帧数据到达前就位。两条 lane 在实现上经 `LaneSession`
抽象承载：`PlainLaneSession` 包裹一条裸 `TunnelSession`（通道号即线协议号），
`ResilientLaneSession` 委托 `ResilientSession`（通道号是跨重连不变的逻辑号）。

  * **会话韧性 opt-in**：`Options::auto_reconnect` 打开后，`open()` 改建
    `ResilientSession`，`fallback_endpoints` 给出按优先级排列的故障转移列表，
    `reconnect_base_delay_ms` / `reconnect_max_delay_ms` 控制指数退避。重连
    成功后 `RemoteSession` 只负责重跑 KOPMS 协商——承载层已把两条 lane 原样
    重开，应用经 `set_reconnect_observer` 收到通知后重新 `hello()`。
  * **序号空间随连接作废**：KOPMS 要求 `sequence` 严格递增。重连的对端是新
    会话、序号从 1 开始，故 `on_transport_state(true)` 在宣布恢复前把
    `send_sequence_` / `last_recv_sequence_` 回到初值；否则重连后首轮
    HELLO/HELLO_ACK 会被“序号未严格递增”误拒，对端回 ERROR 又触发
    `mark_done()`，会话表现为永久死掉。
  * **GOODBYE 的角色语义**：Serve 侧收到 GOODBYE → `mark_done()`；Connect 侧
    收到 GOODBYE 只代表对端要结束当前这条连接，**不**置 `done_`——随后的
    传输 EOF 会经 `on_transport_state(false)` 决定重连或终止。若 Connect 侧
    也按 `mark_done()` 处理，可重连会话将被服务端的一次正常停机永久杀死。


  * **帧提交**：`submit()` 先在 `pending_frames_` 登记 frame id 再发送
    （`FRAME_RELEASE` 可能在 `send()` 返回前就到达），成功时 fd 随该次发送
    移交隧道，失败时由调用方关闭。
  * **帧处置**：`FrameDisposition`——`Release` 立即回送 release；`Retain` 由
    应用 `std::move` 取走帧所有权，稍后显式 `release_frame()`。
  * **断开时的在途状态**：`on_transport_state(false)` 把在途帧一律回放
    `DROPPED`（应用可据此重放）、清空 ack/release 状态、作废协商能力；仅当
    承载层判定永久终止（`lane_->done()`）时才置 `done_` 并放弃通道号。
  * **停机时序**：`disconnect()` 发 GOODBYE 后才 `stop()` 隧道；隧道 worker
    退出前会做一次有界冲刷，保证 GOODBYE 这类停机消息真正写到对端，
    避免对端只能靠传输 EOF 判定结束。传输断开时经 `set_close_handler` 通知
    会话，所有等待者（`wait_for_release` / `wait_done` /
    `wait_for_control_ack`）随即唤醒。

## 应用层 facade（sdk::NetTunnel）

`sdk/include/kop/sdk/net_tunnel.hpp` 把上面三层再向上收一层，给应用/测试
开发者一个只认 **kind 名**的 URI 驱动接口（详见 `docs/sdk.md`）：

- `NetTunnel::open({uri, serve, on_data, on_state, auto_reconnect, …})` 一把
  拉起拨号或服务端角色；拨号端默认走 `ResilientSession`（自动重连 + 退避，
  逻辑通道号跨重连不变），`auto_reconnect=false` 时退化为一次性会话。
- `open_channel(kind, ordered)` / `send(kind|channel, ptr, len)` /
  `close_channel(…)` / `wait_connected()` / `stats()`。
- 服务端角色是“单当前对端”模型，只能接受对端打开的通道（主动开会得到错误）。
- 只搬字节：fd / DMA-BUF 零拷贝仍由 net 节点 + SCM_RIGHTS 承担，facade
  只为节点提供传输。

## 测试

| 目标 | 覆盖 |
|---|---|
| `kopnet-endpoint` | URI 解析（全部 scheme）、语义判断 |
| `kopnet-transport` | 流式/数据报双语义、SCM_RIGHTS、非阻塞、socketpair |
| `kopnet-queue` | 有界队列容量/关闭/跨线程有序；**put 所有权回归** |
| `kopnet-tunnel` | 双端 echo、通道开关、credit 回压（24 包 1KB）、**socket 洪泛回压（64×60KB + 慢消费）**、fd 透传、并发通道 |
| `kopnet-node` | 图级端到端：source 下行 8 包（载荷 + pts/宽高/stride/色彩元数据透传） /
  sink 上行 6 包（同上） / **DMA-BUF 透传：双平面 memfd + acquire fence fd 经
  unix 隧道 SCM_RIGHTS 透传，服务端逐平面 mmap 比对内容** / **断线自动重连 +
  故障转移：杀主服务端后客户端自动切到备用端点，同一逻辑通道继续收帧** |
| `kopnet-resilient` | 断线重连与故障转移（主/备端点、同逻辑通道恢复、状态/通道回调） /
  recv 跨断连挂起并在重连后恢复 / 断连期间注册的通道由连接线程自动打开 /
  重连次数耗尽后停止 |
| `kopnet-remote` | KOPMS 线协议跑在隧道上：参数守卫、HELLO 协商 + ping、FRAME_SUBMIT/fd 透传 + Retain/显式 release 回路、CONTROL 请求/ack、未知类型走 ERROR lane / **断线故障转移：杀主服务端后自动切到备用端点，在途帧回放 DROPPED，重连后重新 hello 并继续提交帧（frame_id 不回卷）** |
| `kopnet-rtp` | rtp:// 回环（首包不丢、16 包有序、stats/SSRC、双向）/ **手搓 RTP 注入 [1,2,5,2,6] 验证丢失 2 + 重复 1** / RTCP 复用跳过且不作为数据投递 / MTU 上限、fd、流式语义拒绝 / **rtp:// 隧道透明性：TunnelSession 双向回显** |
| `kopnet-sdk` | tcp:// 双向回显 + payload 完整 + on_channel / 多通道按 kind 路由（Stream+Datagram）/ 服务端开通道被拒 / **unix:// 自动重连：服务端销毁→同址重建→同一逻辑通道恢复** / 关闭重连即断即止 / close_channel 后发送得明确错误 |

端到端冒烟（手工，记录在变更说明）：
两级中继链 `client → relay → relay → echo server`（5/5 回包），
以及 SSH 适配器路径 `client → ssh(假) → kopnet-relay --stdio → echo server`（3/3 回包）。

```bash
env RUSTUP_TOOLCHAIN=nightly cmake --build --preset relwithdebinfo --target kopnet
ctest -R kopnet                 # 在 build/relwithdebinfo 下
```

## 后续

- [x] `kopnet/remote/remote_session.{hpp,cpp}`：KOPMS 远程会话（HELLO 协商、
      帧提交与 Retain/release 回路、控制请求/ack、错误 lane；窗口树/焦点/输入
      注入走 CONTROL lane，帧走 MEDIA lane，复用 BUS2LAYER 语义）
- [x] `net_source`/`net_sink` 的 DMA-BUF 透传端到端验证：帧信封
      （`frame_envelope.{hpp,cpp}`）携带 pts/宽高/stride/fourcc/modifier/
      色彩元数据与平面 fd，经 unix 传输 SCM_RIGHTS 透传，测试逐平面 mmap 比对
- [x] RTP 适配器（`protocol/rtp_transport.{hpp,cpp}`）：RFC 3550 报文分帧，
      会话化 UDP（首包 connect 锁定五元组），16bit 回绕序号统计丢包/重复，
      RTCP/STUN 复用识别并跳过，MTU 守卫；`rtp://` URI 支持 pt/clock/ts_step/
      fps/ssrc 查询参数；`kopnet-rtp` 测试覆盖回环/丢包统计/RTCP 跳过/限制/
      隧道透明性
- [ ] RTC/RDP 适配器：需 DTLS-SRTP/ICE 栈，离线环境不可引入（已注册但返回
      “尚未实现”）
- [x] 重连与多路径：`ResilientSession`（断线自动重连 + Endpoint 列表故障转移，
      逻辑通道跨重连保持），net 节点 `auto_reconnect` 开关接入；
      `kopnet-resilient` / `kopnet-node` 测试覆盖
- [x] 应用层 facade：`sdk::NetTunnel`（URI 驱动、按 kind 名管理逻辑通道、
      拨号端内置自动重连、服务端单当前对端模型）；`kopnet-sdk` 测试覆盖
