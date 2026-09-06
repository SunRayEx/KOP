// 跨 ABI 帧层：OwnedFrame（引用计数 + 池化回收）与全局 FramePool。
//
// 所有权模型（P1）：
// - 帧带引用计数：独占消费与 tee/多出边共享消费统一处理；
// - 池化帧在最后一个引用释放时回到全局空闲桶（按 media_type+容量 分桶），
//   解码输出路径进入稳态后零 malloc；
// - DMA-BUF 帧（memory_type=DMABUF）：最后一个引用释放时关闭 fd（当前生产
//   路径仍以 CPU 帧为主，Handle ABI 已为后续导出路径固定）。
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "kopaw_abi.h"

namespace kopaw {

static_assert(sizeof(uintptr_t) <= sizeof(uint64_t),
              "Kopaw CPU address aliases must fit in dma_buf_handle");

// Media bytes never live in a pointer field of KopawFrame.  SINGLE/CPU frames
// use a process-local address alias; HYBRID/DMABUF frames must be consumed via
// their local FD/driver handle and must not pass through these helpers.
inline uint64_t handle_from_cpu_address(const void* address) noexcept {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
}

inline const uint8_t* cpu_data(const KopawFrame* frame) noexcept {
    if (!frame || frame->memory_type != KOPAW_MEMORY_CPU ||
        frame->dma_buf_handle == 0) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(frame->dma_buf_handle));
}

inline uint8_t* mutable_cpu_data(KopawFrame* frame) noexcept {
    return const_cast<uint8_t*>(cpu_data(frame));
}

// OwnedFrame：KopawFrame 必须是第一个成员（release 回调用指针回转释放整块）。
struct OwnedFrame {
    KopawFrame frame{};
    std::vector<uint8_t> storage;
    std::atomic<uint32_t> refs{1};
    bool pooled = false;
    // GPU/外部内存帧（DMA-BUF、Vulkan 导出）的资源归还钩子：引用归零时先
    // 关闭帧携带的外部 fd，再由生产者归还图像/内存资源（如导出图像池）。
    // 为空表示无额外外部资源（CPU 池化帧 / 一次性帧）。
    void* external_ctx = nullptr;
    void (*external_release)(void* ctx, OwnedFrame* frame) = nullptr;

    static void close_external_fds(KopawFrame& frame);

    static void retain_cb(KopawFrame* f) {
        reinterpret_cast<OwnedFrame*>(f)->refs.fetch_add(1, std::memory_order_relaxed);
    }

    static void release_cb(KopawFrame* f) {
        auto* o = reinterpret_cast<OwnedFrame*>(f);
        if (o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            close_external_fds(o->frame);
            if (o->external_release) {
                o->external_release(o->external_ctx, o);
            } else if (o->pooled) {
                pool_recycle(o);
            } else {
                delete o;
            }
        }
    }

    KopawFrame* ptr() { return &frame; }
    uint8_t* data() { return storage.data(); }

private:
    // 仅 FramePool 可创建 pooled 块
    static void pool_recycle(OwnedFrame* o);
    friend class FramePool;
};

inline void OwnedFrame::close_external_fds(KopawFrame& frame) {
    // A single-plane producer may mirror dma_fd in planes[0]. Close each
    // descriptor once; all external ownership ends at the final release.
    int fds[KOPAW_MAX_DMABUF_PLANES + 2] = {};
    size_t count = 0;
    auto add_fd = [&](int fd) {
        if (fd < 0) return;
        for (size_t i = 0; i < count; ++i) {
            if (fds[i] == fd) return;
        }
        fds[count++] = fd;
    };
    if (frame.memory_type == KOPAW_MEMORY_DMABUF) {
        add_fd(frame.dma_fd);
        const uint32_t planes = frame.plane_count > KOPAW_MAX_DMABUF_PLANES
                                    ? KOPAW_MAX_DMABUF_PLANES
                                    : frame.plane_count;
        for (uint32_t i = 0; i < planes; ++i) add_fd(frame.planes[i].fd);
    }
    if (frame.acquire_fence.kind == KOPAW_SYNC_FENCE_FD) add_fd(frame.acquire_fence.fd);
    for (size_t i = 0; i < count; ++i) close(fds[i]);
}

// 全局帧池：按 (media_type, 存储容量) 分桶的空闲块回收站。
// 生命周期 = 进程，节点销毁后滞留帧仍然安全（帧不回指节点）。
class FramePool {
public:
    friend void OwnedFrame::pool_recycle(OwnedFrame*);
    // 取一帧：优先复用容量匹配的空闲块，否则新建并标记 pooled。
    // 建议单桶上限 32；超过上限后生产端继续新建，释放端直接销毁。
    static OwnedFrame* acquire(int32_t media_type, size_t data_size) {
        {
            std::lock_guard<std::mutex> lk(mx());
            auto& lst = buckets()[{media_type, data_size}];
            if (!lst.empty()) {
                OwnedFrame* o = lst.back();
                lst.pop_back();
                reset(o, media_type, data_size);
                return o;
            }
        }
        OwnedFrame* o = new OwnedFrame();
        o->pooled = true;
        o->storage.resize(data_size);
        reset(o, media_type, data_size);
        return o;
    }

private:
    using Key = std::pair<int32_t, size_t>;

    static std::mutex& mx() {
        static std::mutex m;
        return m;
    }
    static std::map<Key, std::vector<OwnedFrame*>>& buckets() {
        static std::map<Key, std::vector<OwnedFrame*>> b;
        return b;
    }

    static void reset(OwnedFrame* o, int32_t media_type, size_t data_size) {
        o->refs.store(1, std::memory_order_relaxed);
        o->frame.struct_size = sizeof(KopawFrame);
        o->frame.media_type = media_type;
        o->frame.flags = 0;
        o->frame.pts = 0;
        o->frame.dts = 0;
        o->frame.memory_type = KOPAW_MEMORY_CPU;
        o->frame.dma_fd = -1;
        o->frame.plane_count = 0;
        for (auto& plane : o->frame.planes) {
            plane.fd = -1;
            plane.offset = 0;
            plane.stride = 0;
            plane.modifier = 0;
        }
        o->frame.acquire_fence.kind = KOPAW_SYNC_FENCE_NONE;
        o->frame.acquire_fence.fd = -1;
        o->frame.acquire_fence.value = 0;
        o->frame.user_data = nullptr;
        o->frame.dma_buf_handle =
            data_size ? handle_from_cpu_address(o->storage.data()) : 0;
        o->frame.size = data_size;
        o->frame.stride = 0;
        o->frame.drm_fourcc = 0;
        o->frame.retain = &OwnedFrame::retain_cb;
        o->frame.release = &OwnedFrame::release_cb;
    }

    static void recycle(OwnedFrame* o) {
        std::lock_guard<std::mutex> lk(mx());
        auto& lst = buckets()[{o->frame.media_type, o->storage.capacity()}];
        if (lst.size() < 32) {
            lst.push_back(o);
        } else {
            delete o;
        }
    }
};

inline void OwnedFrame::pool_recycle(OwnedFrame* o) { FramePool::recycle(o); }

// 非池化一次性帧（小对象/包帧/测试）
inline OwnedFrame* make_frame(int32_t media_type, int64_t pts, int64_t dts, size_t data_size) {
    auto* o = new OwnedFrame();
    if (data_size > 0) o->storage.resize(data_size);
    o->frame.struct_size = sizeof(KopawFrame);
    o->frame.media_type = media_type;
    o->frame.flags = 0;
    o->frame.pts = pts;
    o->frame.dts = dts;
    o->frame.memory_type = KOPAW_MEMORY_CPU;
    o->frame.dma_fd = -1;
    o->frame.plane_count = 0;
    for (auto& plane : o->frame.planes) plane.fd = -1;
    o->frame.acquire_fence.kind = KOPAW_SYNC_FENCE_NONE;
    o->frame.acquire_fence.fd = -1;
    o->frame.acquire_fence.value = 0;
    o->frame.user_data = nullptr;
    o->frame.dma_buf_handle =
        data_size ? handle_from_cpu_address(o->storage.data()) : 0;
    o->frame.size = data_size;
    o->frame.stride = 0;
    o->frame.drm_fourcc = 0;
    o->frame.retain = &OwnedFrame::retain_cb;
    o->frame.release = &OwnedFrame::release_cb;
    return o;
}

// 外部内存帧骨架（P3-M3）：无 CPU storage，生产者随后填入
// memory_type（DMABUF / VULKAN）、planes[]、acquire_fence 和 dma_buf_handle，
// 并挂接 external_release 归还生产者资源。外部 fd 由 release 统一关闭。
inline OwnedFrame* make_external_frame(int32_t media_type, int64_t pts, int64_t dts) {
    OwnedFrame* o = make_frame(media_type, pts, dts, 0);
    o->frame.memory_type = KOPAW_MEMORY_DMABUF;
    o->frame.dma_buf_handle = 0;
    return o;
}

// 帧是否携带跨进程可导入的外部平面（DMA-BUF 或 Vulkan 导出）。
inline bool frame_has_external_planes(const KopawFrame* frame) noexcept {
    return frame && (frame->memory_type == KOPAW_MEMORY_DMABUF ||
                     frame->memory_type == KOPAW_MEMORY_VULKAN);
}

// DRM_FORMAT_MOD_INVALID：生产者未提供显式 modifier。允许隐式布局的消费者可走
// 无 modifier 路径；KOPAW 的严格 YCbCr DMA-BUF 导入会拒绝此值。
constexpr uint64_t kDrmFormatModInvalid = 0x00ffffffffffffffull;
// DRM_FORMAT_MOD_LINEAR：所有导入方（含软件渲染）都支持的线性 modifier。
constexpr uint64_t kDrmFormatModLinear = 0;
// DRM_FORMAT_NV12（fourcc 'N','V','1','2'）：两平面 8bit YCbCr 4:2:0，
// VAAPI 解码表面原生导出的标准格式（P2 硬解零拷贝）。
constexpr uint32_t kDrmFormatNv12 = 0x3231564Eu;
// DRM_FORMAT_P010（fourcc 'P','0','1','0'）：两平面 10bit YCbCr 4:2:0
// （16bit 容器高位对齐），HEVC Main10 等硬解的原生导出格式。
constexpr uint32_t kDrmFormatP010 = 0x30313050u;

}  // namespace kopaw
