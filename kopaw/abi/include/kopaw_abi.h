#ifndef KOPAW_ABI_H
#define KOPAW_ABI_H


// 自动生成（cbindgen），勿手改；重新生成命令见 kopaw/core/cbindgen.toml。

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif

struct KopawGraph;


#define KOPAW_ABI_MAJOR 5

/**
 * 5.1: KOPAW_MEMORY_VULKAN external-memory frames (P3-M3). The frame layout
 * is unchanged; only the new memory-type value and capability bit were added.
 */
#define KOPAW_ABI_MINOR 1

#define KOPAW_ABI_VERSION 327681

#define KOPAW_CAP_FRAME_OWNERSHIP 1

#define KOPAW_CAP_NODE_LIFECYCLE 2

#define KOPAW_CAP_MULTI_OUTPUT 4

#define KOPAW_CAP_SELF_DRIVEN 8

#define KOPAW_CAP_DMABUF_FRAMES 16

#define KOPAW_CAP_POOL_SCHEDULER 32

#define KOPAW_CAP_NODE_OUTPUT_BINDING 64

/**
 * Media memory is addressed by the opaque dma_buf_handle field.  CPU-mode
 * consumers may decode it as a local address; it must never cross a process.
 */
#define KOPAW_CAP_HANDLE_FRAMES 128

/**
 * Frames whose memory is a Vulkan device allocation with exportable external
 * memory. In-process consumers use the producer's VkImage; cross-process
 * consumers import the exported DMA-BUF plane described in planes[].
 */
#define KOPAW_CAP_VULKAN_EXTERNAL 256

#define KOPAW_ABI_CAPABILITIES 511

/**
 * 帧标志位
 */
#define KOPAW_FRAME_FLAG_EOS (1 << 0)

/**
 * 解码输入帧：关键帧（demuxer 从 AV_PKT_FLAG_KEY 映射）
 */
#define KOPAW_FRAME_FLAG_KEY (1 << 1)

/**
 * 帧内存类型
 */
#define KOPAW_MEMORY_CPU 0

/**
 * DMA-BUF 外部内存：dma_buf_handle 和 DMA-BUF 平面有效；size 由格式决定，
 * 最后一个 release 负责关闭 fd。
 */
#define KOPAW_MEMORY_DMABUF 1

/**
 * Vulkan external memory：内存由生产者的 Vulkan 设备持有并带 DMA-BUF 导出。
 * dma_buf_handle 是生产者本地的 VkImage 句柄（仅同进程消费有效），跨进程
 * 消费必须导入 planes[] 中的 DMA-BUF fd；fd 同样由最后一个 release 关闭。
 */
#define KOPAW_MEMORY_VULKAN 2

/**
 * DMA-BUF 平面上限；多平面格式可用 planes[] 描述。
 */
#define KOPAW_MAX_DMABUF_PLANES 4

/**
 * 同步栅栏类型。
 */
#define KOPAW_SYNC_FENCE_NONE 0

#define KOPAW_SYNC_FENCE_FD 1

#define KOPAW_SYNC_FENCE_TIMELINE 2

/**
 * 返回码：send / run / emit / recv / connect / start / stop 共用
 */
#define KOPAW_OK 0

#define KOPAW_E_STOPPED 1

#define KOPAW_E_TIMEOUT 2

#define KOPAW_E_EOS 3

#define KOPAW_E_INVALID 4

#define KOPAW_E_GENERIC 5

/**
 * 图状态
 */
#define KOPAW_STATE_IDLE 0

#define KOPAW_STATE_RUNNING 1

#define KOPAW_STATE_STOPPING 2

#define KOPAW_STATE_FINISHED 3

#define KOPAW_STATE_ERROR 4

typedef int32_t KopawMediaType;

typedef struct KopawVideoFormat {
  uint32_t width;
  uint32_t height;
} KopawVideoFormat;

typedef struct KopawAudioFormat {
  uint32_t sample_rate;
  uint32_t channels;
} KopawAudioFormat;

typedef union KopawFormat {
  struct KopawVideoFormat video;
  struct KopawAudioFormat audio;
} KopawFormat;

/**
 * DMA-BUF 平面元数据。fd、offset、stride 和 modifier 的所有权由帧的
 * release 回调管理；消费者不得自行关闭 fd。
 */
typedef struct KopawDmabufPlane {
  int32_t fd;
  uint32_t offset;
  uint32_t stride;
  uint64_t modifier;
} KopawDmabufPlane;

/**
 * 帧进入合成器前需要等待的同步对象。FD 的生命周期同样由 release 管理。
 */
typedef struct KopawSyncFence {
  uint32_t kind;
  int32_t fd;
  uint64_t value;
} KopawSyncFence;

/**
 * 跨 ABI 传递的媒体帧。所有权模型：**引用计数 + 独占释放**。
 *
 * - 生产者分配（refs 从 1 开始）；队列/引擎只搬运指针，不解引用数据；
 * - 单消费者（独占转移）场景：消费完调用 `release(frame)`；
 * - 多消费者（tee/多出边）：引擎在向后续链路投递前调用 `retain(frame)`，
 *   每个消费者各自 release，最后一个引用真正释放。**retain 必须非空**
 *   （引擎检测到多链路且 retain 缺失时会丢弃帧并返回 E_INVALID）；
 * - 引擎在停止与析构时对队列中滞留的帧统一调用 release，不丢不漏；
 * - `dma_buf_handle` 是唯一的媒体内存标识：KOPAW_MEMORY_CPU 下它是当前
 *   地址空间内的地址别名，KOPAW_MEMORY_DMABUF 下它是本地 DMA-BUF 句柄，
 *   KOPAW_MEMORY_VULKAN 下它是生产者本地的 VkImage 句柄；
 *   它不能作为跨进程的数字 FD 使用，跨进程传输必须走拥有 FD 的协议层。
 */
typedef struct KopawFrame {
  /**
   * 必须设置为 sizeof(KopawFrame) 或不小于宿主所需的前缀大小。
   */
  uint32_t struct_size;
  KopawMediaType media_type;
  uint32_t flags;
  /**
   * 微秒级媒体时间戳（展示时间）
   */
  int64_t pts;
  /**
   * 微秒级解码时间戳（B 帧重排；无意义时等于 pts）
   */
  int64_t dts;
  union KopawFormat format;
  /**
   * 媒体内存句柄。CPU 帧为当前地址空间内的地址别名，DMA-BUF 帧为
   * 本地句柄；不要把这个整数当作跨进程 FD 传递。
   */
  uint64_t dma_buf_handle;
  /**
   * 数据字节数
   */
  uintptr_t size;
  /**
   * 视频每行字节数；音频为 0
   */
  uint32_t stride;
  /**
   * 内存类型：KOPAW_MEMORY_CPU / KOPAW_MEMORY_DMABUF
   */
  uint32_t memory_type;
  /**
   * DMA-BUF 文件描述符（CPU 内存时为 -1）
   */
  int32_t dma_fd;
  /**
   * DMA-BUF 平面；CPU 帧为 0。单平面帧可同时填 dma_fd 和 planes[0]。
   */
  uint32_t plane_count;
  struct KopawDmabufPlane planes[4];
  struct KopawSyncFence acquire_fence;
  /**
   * 生产者私有上下文，不由引擎解释。
   */
  void *user_data;
  /**
   * 增加一个引用（tee/多出边投递时由引擎调用）
   */
  void (*retain)(struct KopawFrame *frame);
  /**
   * 释放回调：引用归零时释放帧头及所属数据块。由生产者实现，
   * 引擎与消费者都会调用。
   */
  void (*release)(struct KopawFrame *frame);
} KopawFrame;

/**
 * 输出端口句柄（emit 的寻址凭据）。
 */
typedef struct KopawOutput {
  uint64_t id;
} KopawOutput;

/**
 * 节点实现侧回调（由 C++ 实现）。
 *
 * 所有权约定（send）：
 * - 返回 KOPAW_OK：节点接管帧，之后由节点调用 release；
 * - 返回非 0：节点未接管，引擎立即 release。
 *
 * 注意：函数指针类型必须内联书写（不要经 type alias 间接引用），
 * 否则 cbindgen 无法将其解析为可空 C 函数指针。
 */
typedef struct KopawNodeVTable {
  uint32_t struct_size;
  /**
   * 源节点/自驱动节点的主体循环，返回即视为节点退出
   */
  int32_t (*run)(void *user);
  /**
   * 响应式节点收到一帧（所有权转移见上）
   */
  int32_t (*send)(void *user, struct KopawFrame *frame);
  /**
   * 停止提示：节点应尽快让阻塞点退出
   */
  void (*stop)(void *user);
  /**
   * 图析构时调用，节点释放自身资源（此时刻节点线程必然已全部退出）
   */
  void (*destroy)(void *user);
  /**
   * 图注册后绑定输出端口句柄；没有输出端口的节点可省略。
   */
  void (*bind_output)(void *user, uint32_t port, struct KopawOutput output);
} KopawNodeVTable;

typedef struct KopawNodeDesc {
  uint32_t struct_size;
  /**
   * 诊断用名称（引擎会拷贝）
   */
  const char *name;
  void *user_data;
  /**
   * 输出端口数（0 = 纯汇聚节点）
   */
  uint32_t outputs;
  /**
   * 输入端口数（P2）：响应式节点必须为 1；自驱动节点可声明多个（按端口 recv）
   */
  uint32_t inputs;
  /**
   * 每条入边的有界队列容量
   */
  uint32_t queue_capacity;
  /**
   * 汇聚节点：需自行经 kopaw_node_sink_done 记账；全部汇聚完成 → 图 Finished
   */
  uint8_t is_sink;
  /**
   * 自驱动：引擎不代为 pop，节点在 run() 中用 kopaw_node_recv(_port) 拉取
   */
  uint8_t self_driven;
  const struct KopawNodeVTable *vtable;
} KopawNodeDesc;

typedef int32_t KopawEventType;

typedef struct KopawAbiInfo {
  uint32_t struct_size;
  uint32_t major;
  uint32_t minor;
  /**
   * Capabilities required by a plugin, or provided by the host ABI.
   */
  uint64_t capabilities;
} KopawAbiInfo;

#define KOPAW_MEDIA_VIDEO 0

#define KOPAW_MEDIA_AUDIO 1

#define KOPAW_EVENT_FINISHED 0

#define KOPAW_EVENT_ERROR 1

KopawGraph *kopaw_graph_new(void);

/**
 * 析构：先停止并回收全部线程与滞留帧，再逐节点调用 destroy。
 * 传入空指针安全。
 */
void kopaw_graph_free(KopawGraph *g);

uint32_t kopaw_graph_add_node(KopawGraph *g, const struct KopawNodeDesc *desc);

struct KopawOutput kopaw_graph_node_output(KopawGraph *g, uint32_t node, uint32_t port);

/**
 * 连接：src 输出端口 → dst 节点（容量 cap 的有界队列）。
 * MVP 约束：每个输出端口最多一条出边；非自驱动节点的入边数必须为 1。
 */
int32_t kopaw_graph_connect(KopawGraph *g,
                            struct KopawOutput src,
                            uint32_t dst_node,
                            uint32_t dst_port,
                            uint32_t cap);

int32_t kopaw_graph_start(KopawGraph *g);

/**
 * 请求停止并等待全部节点线程退出。幂等。
 */
int32_t kopaw_graph_stop(KopawGraph *g, uint32_t timeout_ms);

void kopaw_graph_set_event_cb(KopawGraph *g, void (*cb)(void *user,
                                                        KopawEventType ev,
                                                        int32_t code,
                                                        const char *msg), void *user);

int32_t kopaw_graph_state(KopawGraph *g);

/**
 * 节点向指定输出端口发射帧（阻塞式背压）。
 * 图停止时释放帧并返回 KOPAW_E_STOPPED。输出端口未连接时直接释放帧（丢弃）。
 */
int32_t kopaw_graph_emit(KopawGraph *g,
                         struct KopawOutput out,
                         struct KopawFrame *frame);

/**
 * 自驱动节点拉取：从其指定输入端口取一帧（所有权转移到调用方）。
 * 返回的帧可能带 EOS 标志；调用方负责 release。port=0 等价 kopaw_node_recv。
 */
int32_t kopaw_node_recv_port(KopawGraph *g,
                             uint32_t node,
                             uint32_t port,
                             struct KopawFrame **out_frame,
                             uint32_t timeout_ms);

/**
 * 自驱动节点拉取（单入边节点等价 recv_port 端口 0）。保留兼容。
 */
int32_t kopaw_node_recv(KopawGraph *g,
                        uint32_t node,
                        struct KopawFrame **out_frame,
                        uint32_t timeout_ms);

/**
 * 调度模式：workers=0 → 每节点一线程（默认）；workers>0 → 共享队列+工作窃取
 * 线程池（响应式节点以串行化 drain 任务执行；源/自驱动节点仍各占一线程）。
 * 必须在 start 前调用。返回实际生效的 worker 数。
 */
uint32_t kopaw_graph_set_workers(KopawGraph *g,
                                 uint32_t workers);

/**
 * 媒体时钟写入（音频汇聚节点在回调线程调用）
 */
void kopaw_graph_clock_set(KopawGraph *g, int64_t media_us);

int64_t kopaw_graph_clock_get(KopawGraph *g);

/**
 * 时钟是否已被音频路径激活；未激活时视频渲染按墙上时钟节拍。
 */
int32_t kopaw_graph_clock_active(KopawGraph *g);

/**
 * 汇聚节点完成记账（P1：延迟记账协议）。
 * 引擎不再在 EOS 帧交付时自动计数——需要尾后处理（如音频环形缓冲排空）的
 * 汇聚节点在真正完成时自行调用；视频汇聚在最后一帧绘制后调用。
 * 仅汇聚节点有效；重复调用安全（只会触发一次 FINISHED）。
 */
int32_t kopaw_node_sink_done(KopawGraph *g,
                             uint32_t node);

/**
 * 性能基线：导出每节点统计与链路占用（JSON）。
 * 返回写入 buf 的字节数；buf 为空或容量不足时返回所需字节数（含 NUL，取负值 -n）。
 */
int32_t kopaw_graph_stats_json(KopawGraph *g,
                               char *buf,
                               uint32_t cap);

/**
 * 打包 ABI 版本：高 16 位为主版本，低 16 位为次版本。
 */
uint32_t kopaw_abi_version(void);

/**
 * 主机 ABI 信息；插件加载器使用它进行版本、能力和结构体大小协商。
 */
struct KopawAbiInfo kopaw_abi_info(void);

const char *kopaw_version(void);

#ifdef __cplusplus
}
#endif

#endif  /* KOPAW_ABI_H */
