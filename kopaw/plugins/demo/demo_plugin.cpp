// KOPAW demo 插件：演示 .so 动态节点包。
// 节点 1 frame_counter：视频透传 + 周期性 FPS 日志（诊断用）
// 节点 2 flip_video：  垂直翻转 RGBA 帧（演示真实处理路径）
//
// 构建：-fPIC -shared；运行期由宿主可执行文件导出的 kopaw_graph_* 符号解析
// （player 需 ENABLE_EXPORTS/-rdynamic）。
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

#include "kopaw_plugin.h"
#include "kop/log.h"
#include "frame.hpp"

using namespace kopaw;  // NOLINT

static const char* kTag = "demo-plugin";

// ---------------------------------------------------------------------------
// 公共：实例头（两种节点共用）
// ---------------------------------------------------------------------------
struct DemoNode {
    KopawGraph* g = nullptr;
    KopawOutput out{};
    int kind = 0;  // 0 = frame_counter, 1 = flip_video
    // counter 状态
    std::atomic<uint64_t> frames{0};
    std::atomic<int64_t> last_log{0};
};

static int32_t forward(DemoNode* n, KopawFrame* f) {
    // emit consumes the input reference even when the graph is stopping.
    // send must report success so the engine does not release it twice.
    kopaw_graph_emit(n->g, n->out, f);
    return KOPAW_OK;
}

static int32_t counter_send(void* user, KopawFrame* f) {
    auto* n = static_cast<DemoNode*>(user);
    const uint64_t c = n->frames.fetch_add(1) + 1;
    if (f->flags & KOPAW_FRAME_FLAG_EOS) {
        KOP_LOG_INFO(kTag, "frame_counter: 共 %lu 帧", c);
        f->release(f);
        KopawFrame* eos = make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0)->ptr();
        eos->flags |= KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(n->g, n->out, eos);
        return KOPAW_OK;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (now - n->last_log.load() >= 2000) {
        n->last_log.store(now);
        KOP_LOG_INFO(kTag, "frame_counter: %lu 帧（pts=%ld µs）", c, f->pts);
    }
    return forward(n, f);  // 透传（帧所有权移交给引擎）
}

static int32_t flip_send(void* user, KopawFrame* f) {
    auto* n = static_cast<DemoNode*>(user);
    if (f->flags & KOPAW_FRAME_FLAG_EOS) {
        f->release(f);
        KopawFrame* eos = make_frame(KOPAW_MEDIA_VIDEO, 0, 0, 0)->ptr();
        eos->flags |= KOPAW_FRAME_FLAG_EOS;
        kopaw_graph_emit(n->g, n->out, eos);
        return KOPAW_OK;
    }
    // 垂直翻转：行序反转拷入池化新帧
    const uint32_t w = f->format.video.width;
    const uint32_t h = f->format.video.height;
    const uint8_t* input = cpu_data(f);
    if (f->memory_type != KOPAW_MEMORY_CPU || !input || w == 0 || h == 0 ||
        w > UINT32_MAX / 4) {
        f->release(f);
        return KOPAW_OK;
    }
    const uint32_t row = w * 4;
    const size_t required = static_cast<size_t>(f->stride) * h;
    if (f->stride < row || (h != 0 && required / h != f->stride) || f->size < required ||
        static_cast<size_t>(row) > SIZE_MAX / h) {
        f->release(f);
        return KOPAW_OK;
    }
    OwnedFrame* o = FramePool::acquire(KOPAW_MEDIA_VIDEO,
                                       static_cast<size_t>(row) * h);
    o->frame.pts = f->pts;
    o->frame.dts = f->dts;
    o->frame.format.video.width = w;
    o->frame.format.video.height = h;
    o->frame.stride = row;
    for (uint32_t y = 0; y < h; ++y) {
        memcpy(o->data() + static_cast<size_t>(y) * row,
               input + static_cast<size_t>(h - 1 - y) * f->stride, row);
    }
    f->release(f);
    kopaw_graph_emit(n->g, n->out, o->ptr());
    return KOPAW_OK;
}

// vtable（静态；两种节点共用 stop/destroy，send 按实例分发）
static int32_t demo_send(void* user, KopawFrame* f) {
    auto* n = static_cast<DemoNode*>(user);
    return n->kind == 0 ? counter_send(user, f) : flip_send(user, f);
}

static void demo_bind_output(void* user, uint32_t port, KopawOutput output) {
    auto* n = static_cast<DemoNode*>(user);
    if (port == 0) n->out = output;
}

static KopawNodeVTable demo_vt() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.send = demo_send;
    vt.stop = [](void*) {};
    vt.destroy = [](void* user) { delete static_cast<DemoNode*>(user); };
    vt.bind_output = demo_bind_output;
    return vt;
}
static const KopawNodeVTable kVt = demo_vt();

// ---------------------------------------------------------------------------
// 描述与实例化
// ---------------------------------------------------------------------------
static KopawNodeDesc kDescs[2] = {};

static const KopawNodeDesc* demo_desc(uint32_t i) {
    if (i > 1) return nullptr;
    KopawNodeDesc& d = kDescs[i];
    d = KopawNodeDesc{};
    d.struct_size = sizeof(d);
    d.name = i == 0 ? "frame_counter" : "flip_video";
    d.outputs = 1;
    d.inputs = 1;
    d.queue_capacity = 8;
    d.is_sink = 0;
    d.self_driven = 0;
    d.vtable = &kVt;
    return &d;
}

static int32_t demo_create(uint32_t i, KopawGraph* g, KopawNodeDesc* out) {
    if (i > 1) return KOPAW_E_INVALID;
    auto* inst = new DemoNode();
    inst->g = g;
    inst->kind = static_cast<int>(i);
    *out = *demo_desc(i);
    out->user_data = inst;
    return KOPAW_OK;
}

KOPAW_PLUGIN_EXPORTS(demo_create, "kopaw-demo-plugin", 2, demo_desc, demo_create)
