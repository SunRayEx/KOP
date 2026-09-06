// P3-M3/M4 基准：DMA-BUF 零拷贝路径的延迟、CPU 占用、帧丢失、导入耗时与
// fence 等待耗时。
//
// 场景：本进程内起真实 KOPMS-S（BUS2LAYER seqpacket + SCM_RIGHTS）与
// Vulkan 场景，KOPMS-C 以 KOPAW 导出器产出 KOPAW_MEMORY_VULKAN 帧提交：
//   阶段一（吞吐）：每帧独立场景窗口 → handler Retain → render（fence 节拍）
//                    → release 回 FRAME_RELEASE，测 submit→release 端到端延迟；
//   阶段二（回压）  ：全部帧打向同一窗口、提交先于渲染 → 场景拒收 →
//                    DROPPED，测帧丢失与回压路径开销。
// 输出机器可读 JSON。无 Vulkan DMA-BUF 能力时退出码 77。
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <ctime>
#include <thread>
#include <string>
#include <vector>

#include "frame.hpp"
#include "frame_bridge.h"
#include "kopms_client.h"
#include "kopms_server.h"
#include "render/vulkan/vk_dma_export.hpp"
#include "vk_scene.hpp"
#include "wayland-server-core.h"

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 240;
constexpr int kFrames = 120;

constexpr uint64_t kThroughputWindowBase = 0x7000000000000000ull;
constexpr uint64_t kBackpressureWindow = 0x6000000000000000ull;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void pump_server(wl_event_loop* loop) {
    for (int i = 0; i < 4; ++i) wl_event_loop_dispatch(loop, 0);
}

struct SendRef {
    KopawFrame* frame;
};

// descriptor 必须堆分配：KopmsClient 在整个在飞期间持有其指针，
// FRAME_RELEASE 的 release 回调负责回收 descriptor 与自持引用。
KopmsFrameDescriptor* make_descriptor(KopawFrame* gpu) {
    auto* desc = new KopmsFrameDescriptor{};
    desc->struct_size = sizeof(*desc);
    desc->version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    desc->media_type = KOPAW_MEDIA_VIDEO;
    desc->width = gpu->format.video.width;
    desc->height = gpu->format.video.height;
    desc->format = 0x34324241u;  // AB24（R8G8B8A8 的 DRM fourcc）
    desc->memory_type = KOPAW_MEMORY_VULKAN;
    desc->plane_count = gpu->plane_count;
    desc->planes[0] = gpu->planes[0];
    desc->acquire_fence = gpu->acquire_fence;
    desc->dma_buf_handle = static_cast<uint32_t>(gpu->planes[0].fd);
    desc->user_data = new SendRef{gpu};
    desc->retain = [](KopmsFrameDescriptor* d) {
        auto* f = static_cast<SendRef*>(d->user_data)->frame;
        f->retain(f);
    };
    desc->release = [](KopmsFrameDescriptor* d) {
        auto* ref = static_cast<SendRef*>(d->user_data);
        ref->frame->release(ref->frame);
        delete ref;
        delete d;
    };
    return desc;
}

kopms::VulkanScene::ImportRequest request_from(const kopms::KopmsReceivedFrame& frame) {
    kopms::VulkanScene::ImportRequest request{};
    request.width = frame.payload.width;
    request.height = frame.payload.height;
    request.format = frame.payload.format;
    request.plane_count = frame.payload.plane_count;
    static thread_local int fds[KOPAW_MAX_DMABUF_PLANES] = {-1, -1, -1, -1};
    static thread_local uint32_t offsets[KOPAW_MAX_DMABUF_PLANES] = {};
    static thread_local uint32_t strides[KOPAW_MAX_DMABUF_PLANES] = {};
    static thread_local uint64_t modifiers[KOPAW_MAX_DMABUF_PLANES] = {};
    for (uint32_t i = 0;
         i < frame.payload.plane_count && i < KOPAW_MAX_DMABUF_PLANES; ++i) {
        fds[i] = frame.fds[static_cast<size_t>(frame.payload.planes[i].fd_index)];
        offsets[i] = frame.payload.planes[i].offset;
        strides[i] = frame.payload.planes[i].stride;
        modifiers[i] = frame.payload.planes[i].modifier;
    }
    request.plane_fds = fds;
    request.plane_offsets = offsets;
    request.plane_strides = strides;
    request.plane_modifiers = modifiers;
    request.acquire_fence_kind = frame.payload.acquire_fence.kind;
    request.acquire_fence_fd =
        frame.payload.acquire_fence.kind == KOPAW_SYNC_FENCE_FD
            ? frame.fds[static_cast<size_t>(frame.payload.acquire_fence.fd_index)]
            : -1;
    request.session_id = frame.session_id;
    request.frame_id = frame.payload.frame_id;
    return request;
}

kopaw::OwnedFrame* make_cpu_frame() {
    kopaw::OwnedFrame* cpu = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0,
                                               static_cast<size_t>(kW) * kH * 4);
    cpu->frame.format.video.width = kW;
    cpu->frame.format.video.height = kH;
    cpu->frame.stride = kW * 4;
    std::memset(cpu->data(), 0x5a, static_cast<size_t>(kW) * kH * 4);
    return cpu;
}

// 阶段一：每帧独立窗口；frame i 的 FRAME_RELEASE 在 render(i+1) 的 fence
// 完成后到达 → 延迟覆盖“导出 + BUS + 导入 + 合成 + fence”全链。
bool run_throughput(kopms::KopmsServer* server, kopms::KopmsClient* client,
                    kopms::VulkanScene* scene, kopaw::VulkanDmabufExporter* exporter,
                    wl_event_loop* loop, std::vector<int64_t>* latency_us,
                    std::vector<int64_t>* wire_import_us, std::string* error, int frames) {
    server->set_frame_handler(
        [scene](kopms::KopmsReceivedFrame& frame) -> kopms::FrameDisposition {
            static thread_local uint64_t window_seq = kThroughputWindowBase;
            ++window_seq;
            std::string ignored;
            if (!scene->submit(window_seq, request_from(frame), &ignored)) {
                return kopms::FrameDisposition::ReleaseDropped;
            }
            return kopms::FrameDisposition::Retain;
        });
    scene->set_release_callback(
        [server](uint64_t, uint64_t session, uint32_t frame_id) {
            server->release_frame(session, frame_id);
        });

    kopaw::OwnedFrame* cpu = make_cpu_frame();
    std::vector<uint32_t> frame_ids;
    std::vector<int64_t> submit_at;
    bool ok = true;
    for (int i = 0; i < frames && ok; ++i) {
        KopawFrame* gpu = exporter->export_cpu_frame(&cpu->frame, error);
        if (!gpu) {
            *error = "export failed: " + *error;
            ok = false;
            break;
        }
        KopmsFrameDescriptor* desc = make_descriptor(gpu);
        desc->pts = i;
        submit_at.push_back(now_us());
        uint32_t frame_id = 0;
        if (!client->submit(desc, &frame_id, error)) {
            // submit 失败：client 已触发 desc->release（回收 desc 并归还
            // 自持引用），这里只需丢弃本人引用。
            gpu->release(gpu);
            ok = false;
            break;
        }
        frame_ids.push_back(frame_id);
        pump_server(loop);
        wire_import_us->push_back(now_us() - submit_at.back());

        // 合成第 i 帧；fence 完成后释放第 i-1 帧 → FRAME_RELEASE 到达。
        kopms::VulkanScene::LayoutItem item;
        item.window_id = kThroughputWindowBase + static_cast<uint64_t>(i) + 1;
        item.z = 0;
        item.rect = {0, 0, kW, kH};
        std::vector<kopms::VulkanScene::LayoutItem> layout{item};
        if (!scene->render(layout, error)) {
            gpu->release(gpu);
            ok = false;
            break;
        }
        pump_server(loop);
        if (frame_ids.size() >= 2) {
            const size_t prev = frame_ids.size() - 2;
            const int64_t deadline = now_us() + 2'000'000;
            std::string ignored;
            for (;;) {
                if (client->wait_for_release(frame_ids[prev], 2, &ignored)) {
                    latency_us->push_back(now_us() - submit_at[prev]);
                    break;
                }
                pump_server(loop);
                if (now_us() > deadline) {
                    *error = "FRAME_RELEASE timeout";
                    ok = false;
                    break;
                }
            }
        }
        gpu->release(gpu);
    }
    // 最后一帧：空渲染触发其释放。
    if (ok && !frame_ids.empty()) {
        std::vector<kopms::VulkanScene::LayoutItem> empty;
        if (!scene->render(empty, error)) {
            cpu->release_cb(&cpu->frame);
            return false;
        }
        pump_server(loop);
        const size_t last = frame_ids.size() - 1;
        const int64_t deadline = now_us() + 2'000'000;
        std::string ignored;
        for (;;) {
            if (client->wait_for_release(frame_ids[last], 2, &ignored)) {
                latency_us->push_back(now_us() - submit_at[last]);
                break;
            }
            pump_server(loop);
            if (now_us() > deadline) {
                *error = "FRAME_RELEASE timeout (last frame)";
                ok = false;
                break;
            }
        }
    }
    cpu->release_cb(&cpu->frame);
    return ok;
}

// 阶段二：同一窗口高速提交（提交先于渲染）→ 回压拒收。
bool run_backpressure(kopms::KopmsServer* server, kopms::KopmsClient* client,
                      kopms::VulkanScene* scene,
                      kopaw::VulkanDmabufExporter* exporter, wl_event_loop* loop,
                      int* dropped, int* accepted, std::string* error) {
    const uint64_t rejected_before = scene->stats().rejected_frames;
    const uint64_t imported_before = scene->stats().imported_frames;
    server->set_frame_handler(
        [scene](kopms::KopmsReceivedFrame& frame) -> kopms::FrameDisposition {
            std::string ignored;
            if (!scene->submit(kBackpressureWindow, request_from(frame), &ignored)) {
                return kopms::FrameDisposition::ReleaseDropped;
            }
            return kopms::FrameDisposition::Retain;
        });

    kopaw::OwnedFrame* cpu = make_cpu_frame();
    bool ok = true;
    for (int i = 0; i < 40 && ok; ++i) {
        KopawFrame* gpu = exporter->export_cpu_frame(&cpu->frame, error);
        if (!gpu) {
            *error = "export failed: " + *error;
            ok = false;
            break;
        }
        KopmsFrameDescriptor* desc = make_descriptor(gpu);
        uint32_t frame_id = 0;
        if (!client->submit(desc, &frame_id, error)) {
            gpu->release(gpu);
            ok = false;
            break;
        }
        pump_server(loop);
        pump_server(loop);
        // 消费 FRAME_RELEASE（OK/DROPPED），让客户端引用与图像池流动。
        {
            std::string ignored;
            for (int k = 0; k < 4 && client->dispatch(0, &ignored); ++k) {
            }
        }
        gpu->release(gpu);
        // 偶数迭代不渲染：下一次 submit 撞上仍在飞的上一帧 → 场景拒收
        // → DROPPED（帧回压路径的真实触发）。
        if (i % 2 == 1) {
            kopms::VulkanScene::LayoutItem item;
            item.window_id = kBackpressureWindow;
            item.rect = {0, 0, kW, kH};
            std::vector<kopms::VulkanScene::LayoutItem> layout{item};
            if (!scene->render(layout, error)) {
                ok = false;
                break;
            }
            pump_server(loop);
            std::string ignored;
            for (int k = 0; k < 4 && client->dispatch(0, &ignored); ++k) {
            }
        }
    }
    // 排空最后一批释放（多渲染一轮直到没有 pending/presented）。
    if (ok) {
        for (int i = 0; i < 6; ++i) {
            std::vector<kopms::VulkanScene::LayoutItem> empty;
            if (!scene->render(empty, error)) break;
            pump_server(loop);
            std::string ignored;
            for (int k = 0; k < 8 && client->dispatch(0, &ignored); ++k) {
            }
        }
    }
    const kopms::VulkanScene::Stats& stats = scene->stats();
    *dropped = static_cast<int>(stats.rejected_frames - rejected_before);
    *accepted = static_cast<int>(stats.imported_frames - imported_before);
    cpu->release_cb(&cpu->frame);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = kFrames;
    if (argc > 1) frames = std::atoi(argv[1]);

    std::string reason;
    if (!kopaw::VulkanDmabufExporter::export_supported(&reason)) {
        std::fprintf(stdout, "skip: %s\n", reason.c_str());
        return 77;
    }
    kopaw::VulkanDmabufExporter exporter;
    if (exporter.init(16, &reason)) {
        std::fprintf(stderr, "exporter init failed: %s\n", reason.c_str());
        return 1;
    }
    kopms::VulkanScene scene;
    kopms::VulkanScene::Options scene_options{};
    scene_options.windowed = false;
    scene_options.width = kW;
    scene_options.height = kH;
    if (scene.init(scene_options, nullptr, &reason)) {
        std::fprintf(stderr, "scene init failed: %s\n", reason.c_str());
        return 1;
    }

    wl_event_loop* loop = wl_event_loop_create();
    const std::string socket_path =
        "/tmp/kopms-dmabuf-bench-" + std::to_string(static_cast<long long>(getpid()));
    kopms::KopmsServer server(loop, socket_path, {}, kopms::GpuMode::Hybrid);
    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "server start failed: %s\n", error.c_str());
        return 1;
    }
    kopms::KopmsClient client;
    if (!client.connect(socket_path, &error)) {
        std::fprintf(stderr, "client connect failed: %s\n", error.c_str());
        return 1;
    }
    {
        // hello 内部阻塞等待 ACK：握手期间用临时线程泵服务端事件循环。
        std::atomic<bool> pumping{true};
        std::thread pumper([&] {
            while (pumping.load()) wl_event_loop_dispatch(loop, 2);
        });
        const bool ok = client.hello(
            KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                KOPMS_PROTOCOL_CAP_HANDLE_FRAMES | KOPMS_PROTOCOL_CAP_MODIFIERS |
                KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC,
            &error);
        pumping.store(false);
        pumper.join();
        if (!ok) {
            std::fprintf(stderr, "client hello failed: %s\n", error.c_str());
            return 1;
        }
    }

    std::vector<int64_t> latency_us;
    std::vector<int64_t> wire_import_us;
    const clock_t cpu_start = clock();
    const int64_t wall_start = now_us();
    if (!run_throughput(&server, &client, &scene, &exporter, loop, &latency_us,
                        &wire_import_us, &error, frames)) {
        std::fprintf(stderr, "throughput phase failed: %s\n", error.c_str());
        return 1;
    }
    int dropped = 0;
    int accepted = 0;
    if (!run_backpressure(&server, &client, &scene, &exporter, loop, &dropped,
                          &accepted, &error)) {
        std::fprintf(stderr, "backpressure phase failed: %s\n", error.c_str());
        return 1;
    }
    const clock_t cpu_end = clock();
    const int64_t wall_us = now_us() - wall_start;

    auto avg = [](const std::vector<int64_t>& v) {
        if (v.empty()) return int64_t{0};
        int64_t sum = 0;
        for (int64_t x : v) sum += x;
        return sum / static_cast<int64_t>(v.size());
    };
    auto p99 = [](std::vector<int64_t> v) {
        if (v.empty()) return int64_t{0};
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(v.size() * 99 / 100)];
    };
    const kopms::VulkanScene::Stats stats = scene.stats();

    std::printf("{\n");
    std::printf("  \"frames\": %d,\n", static_cast<int>(latency_us.size()));
    std::printf("  \"resolution\": \"%ux%u\",\n", kW, kH);
    std::printf("  \"submit_to_release_avg_us\": %lld,\n",
                static_cast<long long>(avg(latency_us)));
    std::printf("  \"submit_to_release_p99_us\": %lld,\n",
                static_cast<long long>(p99(latency_us)));
    std::printf("  \"wire_only_avg_us\": %lld,\n",
                static_cast<long long>(avg(wire_import_us)));
    std::printf("  \"scene_import_avg_us\": %lld,\n",
                static_cast<long long>(stats.imported_frames
                                           ? stats.import_us_total / stats.imported_frames
                                           : 0));
    std::printf("  \"fence_wait_total_us\": %llu,\n",
                static_cast<unsigned long long>(stats.fence_wait_us_total));
    std::printf("  \"exporter_upload_avg_us\": %llu,\n",
                static_cast<unsigned long long>(exporter.exported_frames()
                                                    ? exporter.upload_us_total() /
                                                          exporter.exported_frames()
                                                    : 0));
    std::printf("  \"presented_frames\": %llu,\n",
                static_cast<unsigned long long>(stats.presented_frames));
    std::printf("  \"backpressure_dropped\": %d,\n", dropped);
    std::printf("  \"backpressure_accepted\": %d,\n", accepted);
    std::printf("  \"wall_ms\": %.1f,\n", static_cast<double>(wall_us) / 1000.0);
    std::printf("  \"cpu_ms\": %.1f,\n",
                static_cast<double>(cpu_end - cpu_start) * 1000.0 / CLOCKS_PER_SEC);
    std::printf("  \"cpu_percent\": %.1f\n",
                wall_us > 0 ? (static_cast<double>(cpu_end - cpu_start) /
                               static_cast<double>(wall_us)) *
                                  100.0
                            : 0.0);
    std::printf("}\n");

    client.disconnect();
    server.stop();
    scene.shutdown();
    exporter.shutdown();
    wl_event_loop_destroy(loop);
    return 0;
}
