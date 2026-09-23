#include "render_node.hpp"

#include <algorithm>

#include "kop/log.h"
#include "kop/time.h"

namespace kopaw {

static const char* kTag = "render";

namespace {
KopawNodeVTable make_vtable() {
    KopawNodeVTable vt{};
    vt.struct_size = sizeof(vt);
    vt.run = [](void* user) -> int32_t {
        return static_cast<RenderNode*>(user)->run_impl();
    };
    vt.send = nullptr;
    vt.stop = [](void* user) {
        // 停止提示：窗口关闭路径由 player 控制；此处仅标记
        static_cast<RenderNode*>(user)->close_requested().store(true);
    };
    vt.destroy = [](void* user) { delete static_cast<RenderNode*>(user); };
    return vt;
}
const KopawNodeVTable kVTable = make_vtable();
}  // namespace

KopawNodeDesc RenderNode::desc() {
    KopawNodeDesc d{};
    d.struct_size = sizeof(d);
    d.name = "video_render";
    d.user_data = this;
    d.outputs = 0;
    d.inputs = 1;
    d.queue_capacity = 4;  // RGBA 帧大，少量即可
    d.is_sink = 1;
    d.self_driven = 1;
    d.vtable = &kVTable;
    return d;
}

int32_t RenderNode::run_impl() {
    std::string err;
    if (!backend_->init(win_, &err)) {
        KOP_LOG_ERROR(kTag, "渲染后端初始化失败: %s", err.c_str());
        close_requested_.store(true);
        return KOPAW_E_GENERIC;
    }
    KOP_LOG_INFO(kTag, "使用 %s 渲染后端", backend_->name());
    if (dmabuf_capability_callback_) {
        dmabuf_capability_callback_(backend_->dmabuf_format_mask());
    }

    // 音频时钟未激活时以首帧为原点的墙上时钟节拍
    bool anchored = false;
    int64_t wall_anchor = 0;

    auto media_now = [&]() -> int64_t {
        if (kopaw_graph_clock_active(g_)) return kopaw_graph_clock_get(g_);
        if (!anchored) {
            wall_anchor = kop::steady_us();
            anchored = true;
        }
        return kop::steady_us() - wall_anchor;
    };

    while (true) {
        if (backend_->window_closed()) {
            close_requested_.store(true);
            break;
        }
        KopawFrame* f = nullptr;
        int rc = kopaw_node_recv(g_, node_id_, &f, 10);
        if (rc == KOPAW_E_STOPPED) break;
        if (rc == KOPAW_E_TIMEOUT) {
            backend_->poll_events();  // 无新帧也保持窗口响应
            continue;
        }
        if (rc != KOPAW_OK) {
            KOP_LOG_ERROR(kTag, "recv 错误 %d", rc);
            break;
        }
        if (f->flags & KOPAW_FRAME_FLAG_EOS) {
            f->release(f);
            // P1 延迟记账：最后一帧已绘制完成，向引擎记账
            kopaw_node_sink_done(g_, node_id_);
            break;
        }

        // A/V 同步：等待到帧的展示时刻（提前 0.8ms 醒来）
        const int64_t target = f->pts;
        while (media_now() + 800 < target) {
            if (backend_->window_closed()) break;
            int64_t remain = target - media_now() - 800;
            kop::sleep_us(std::min<int64_t>(remain, 2000));
        }
        backend_->poll_events();
        const bool external = f->memory_type == KOPAW_MEMORY_DMABUF;
        bool ok = backend_->draw(f);
        f->release(f);
        if (!ok) {
            if (external && !dmabuf_fallback_used_ && dmabuf_fallback_) {
                dmabuf_fallback_used_ = true;
                KOP_LOG_WARN(kTag,
                             "DMA-BUF 导入失败，关闭解码器原生输出并回退 CPU 路径");
                dmabuf_fallback_();
                continue;
            }
            KOP_LOG_ERROR(kTag, "绘制失败，终止渲染");
            close_requested_.store(true);
            break;
        }
    }

    backend_->shutdown();
    return KOPAW_OK;
}

}  // namespace kopaw
