// kopaw-player：KOPAW MVP 纵向切片 CLI 播放器。
//
// 用法：kopaw-player <file> [--backend vulkan|opengl] [--width W] [--height H]
//                  [--no-audio] [--no-video] [--duration SEC]
//                  [--filter video:<chain>|audio:<chain>] [--plugin path --use node]
//                  [--zero-copy on|off|auto] [--kopms-bus NAME] [--kopms-window ID]
//
// 全链：demuxer → (video_decoder → video_render) / (audio_decoder → audio_sink)
// 音频回调驱动主时钟，视频按 pts 对齐节拍。
// --kopms-bus：视频走 KOPMS 零拷贝路径（Vulkan 导出 DMA-BUF → BUS2LAYER →
// KOPMS-S 合成器），替代本地渲染窗口（P3-M3）。
// --zero-copy：硬解表面原生 DMA-BUF 直通 Vulkan 渲染（P2；auto 仅在
// Vulkan 后端且无滤镜/插件介入时启用，导出失败自动回退软拷贝）。
//
// 所有权约定：任何节点对象一旦 add_node 成功，其析构即由 graph_free 的
// destroy 回调负责；player 不再手动 delete。因此所有可能失败的 open/创建
// 都放在入图之前完成。
#include <atomic>
#include <vector>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <string>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <portaudio.h>

#include "audio/portaudio_sink_node.hpp"
#include "ffmpeg/audio_decoder_node.hpp"
#include "ffmpeg/demuxer_node.hpp"
#include "ffmpeg/video_decoder_node.hpp"
#include "ffmpeg/filter_node.hpp"
#include "kop/log.h"
#include "kop/time.h"
#include "kopaw_abi.h"
#include "render/render_backend.hpp"
#include "plugin_loader.hpp"
#include "render/render_node.hpp"
#if KOPAW_HAVE_KOPMS_SINK
#include "kopms/kopms_sink_node.hpp"
#endif

static const char* kTag = "player";

namespace {
std::atomic<bool> g_finished{false};
std::atomic<bool> g_error{false};

void event_cb(void*, int ev, int code, const char* msg) {
    if (ev == KOPAW_EVENT_FINISHED) {
        KOP_LOG_INFO(kTag, "播放完成");
        g_finished.store(true);
    } else if (ev == KOPAW_EVENT_ERROR) {
        KOP_LOG_ERROR(kTag, "管线错误 code=%d (%s)", code, msg);
        g_error.store(true);
    }
}

void usage() {
    fprintf(stderr,
            "用法: kopaw-player <file> [选项]\n"
            "  --backend vulkan|opengl  渲染后端（默认 vulkan）\n"
            "  --width W   窗口宽（默认 1280）\n"
            "  --height H  窗口高（默认 720）\n"
            "  --no-audio  禁用音频\n"
            "  --no-video  禁用视频\n"
            "  --duration SEC  最长播放媒体秒数后退出\n"
            "  --filter SPEC   滤镜链；video:... 或 audio:...，可重复\n"
            "  --timeout-ms N  网络输入单次 I/O 超时（默认 15000，0 = 不设上限）\n"
            "  --buffer-ms N   网络抖动缓冲（默认 250）\n"
            "  --plugin PATH   加载 .so 插件\n"
            "  --use NODE      将插件节点插入视频链\n"
            "  --zero-copy M   硬解表面原生 DMA-BUF 直通渲染（on|off|auto，默认 auto）\n"
            "  --kopms-bus NAME     视频零拷贝提交到 KOPMS-S 合成器（P3-M3）\n"
            "  --kopms-window ID    目标 CONTROL 窗口（默认 1，自动创建并 attach）\n");
}
}  // namespace

int main(int argc, char** argv) {
    kop::log_set_level(kop::log_level_from_env());

    // ABI 版本校验：Rust 引擎与 C++ 节点库必须基于同一契约构建
    const KopawAbiInfo abi = kopaw_abi_info();
    if (abi.major != KOPAW_ABI_MAJOR || abi.minor > KOPAW_ABI_MINOR ||
        abi.struct_size < sizeof(KopawAbiInfo)) {
        KOP_LOG_ERROR(kTag, "ABI 信息无效（%u.%u，size=%u）", abi.major, abi.minor,
                      abi.struct_size);
        return 3;
    }

    if (argc < 2) {
        usage();
        return 2;
    }
    std::string file = argv[1];
    std::string backend_name = "vulkan";
    int width = 1280, height = 720;
    bool want_audio = true, want_video = true;
    double duration = 0;
    uint32_t timeout_ms = 15000;
    uint32_t buffer_ms = 250;
    std::vector<std::string> plugin_paths;
    std::string use_node;
    std::string video_filter_desc;
    std::string audio_filter_desc;
    std::string zero_copy_mode = "auto";  // on|off|auto
    std::vector<std::pair<std::string, std::string>> input_opts;
#if KOPAW_HAVE_KOPMS_SINK
    std::string kopms_bus;
    uint64_t kopms_window = 1;
    bool kopms_mode = false;
#endif

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--backend") backend_name = next();
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--no-audio") want_audio = false;
        else if (a == "--no-video") want_video = false;
        else if (a == "--duration") duration = std::atof(next().c_str());
        else if (a == "--timeout-ms") timeout_ms = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--buffer-ms") buffer_ms = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--zero-copy") zero_copy_mode = next();
        else if (a == "--plugin") plugin_paths.push_back(next());
        else if (a == "--use") use_node = next();
        else if (a == "--filter") {
            std::string spec = next();
            auto append_filter = [](std::string* dst, const std::string& chain) {
                if (chain.empty()) return;
                if (!dst->empty()) *dst += ",";
                *dst += chain;
            };
            if (spec.rfind("audio:", 0) == 0)
                append_filter(&audio_filter_desc, spec.substr(6));
            else if (spec.rfind("video:", 0) == 0)
                append_filter(&video_filter_desc, spec.substr(6));
            else
                append_filter(&video_filter_desc, spec);
        }
        else if (a == "--opt") {
            std::string kv = next();
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                input_opts.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
#if KOPAW_HAVE_KOPMS_SINK
        else if (a == "--kopms-bus") { kopms_bus = next(); kopms_mode = !kopms_bus.empty(); }
        else if (a == "--kopms-window") kopms_window = std::strtoull(next().c_str(), nullptr, 10);
#endif
        else { usage(); return 2; }
    }

    // ---- 全局子系统 ----
    bool pa_inited = false, glfw_inited = false;
    if (want_audio) {
        if (Pa_Initialize() != paNoError) {
            KOP_LOG_ERROR(kTag, "PortAudio 初始化失败");
            return 1;
        }
        pa_inited = true;
    }
    if (want_video) {
        if (!glfwInit()) {
            KOP_LOG_ERROR(kTag, "GLFW 初始化失败");
            if (pa_inited) Pa_Terminate();
            return 1;
        }
        glfw_inited = true;
        // 窗口按所选后端设置创建提示：Vulkan 无 GL 上下文；OpenGL 3.3 core
        if (backend_name == "vulkan") {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        } else {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        }
    }

    int exit_code = 1;
    KopawGraph* g = nullptr;
    GLFWwindow* win = nullptr;
    std::string err;

#if KOPAW_HAVE_KOPMS_SINK
    kopaw::KopmsSinkNode* kopms_sink = nullptr;
#endif

    // ---- 阶段一：创建并打开全部资源（失败即手动清理，尚未入图） ----
    auto* demux = new kopaw::DemuxerNode();
    kopaw::VideoDecoderNode* vdec = nullptr;
    kopaw::VideoFilterNode* vfilter = nullptr;
    kopaw::AudioDecoderNode* adec = nullptr;
    kopaw::AudioFilterNode* afilter = nullptr;
    kopaw::AudioSinkNode* asink = nullptr;
    kopaw::IRenderBackend* backend = nullptr;
    bool ready = false;

    do {
        demux->set_io_policy(timeout_ms, buffer_ms);
        if (!demux->open(file, input_opts, &err)) {
            KOP_LOG_ERROR(kTag, "%s", err.c_str());
            exit_code = 2;
            break;
        }
        const bool use_video = want_video && demux->has_video();
        const bool use_audio = want_audio && demux->has_audio();
        if (!use_video && !use_audio) {
            KOP_LOG_ERROR(kTag, "没有可播放的流");
            exit_code = 2;
            break;
        }

        if (use_video) {
            vdec = new kopaw::VideoDecoderNode();
            if (!vdec->open(demux->video_codec_params(), &err)) {
                KOP_LOG_ERROR(kTag, "视频解码器: %s", err.c_str());
                exit_code = 2;
                break;
            }
            if (!video_filter_desc.empty()) {
                vfilter = new kopaw::VideoFilterNode();
                if (!vfilter->open(video_filter_desc, &err)) {
                    KOP_LOG_ERROR(kTag, "滤镜节点: %s", err.c_str());
                    exit_code = 2;
                    break;
                }
            }
#if KOPAW_HAVE_KOPMS_SINK
            if (kopms_mode) {
                // P3-M3：视频不落本地窗口，走 KOPMS 零拷贝路径。
                kopaw::KopmsSinkNode::Options opts{};
                opts.bus_socket = kopms_bus;
                opts.window_id = kopms_window;
                kopms_sink = new kopaw::KopmsSinkNode(std::move(opts));
                if (!kopms_sink->connect(&err)) {
                    KOP_LOG_ERROR(kTag, "KOPMS sink 连接失败: %s", err.c_str());
                    exit_code = 2;
                    break;
                }
            } else
#endif
            {
                win = glfwCreateWindow(width, height, "KOPAW Player", nullptr, nullptr);
                if (!win) {
                    KOP_LOG_ERROR(kTag, "窗口创建失败");
                    exit_code = 2;
                    break;
                }
                backend = kopaw::create_render_backend(backend_name, &err);
                if (!backend) {
                    KOP_LOG_ERROR(kTag, "%s", err.c_str());
                    exit_code = 2;
                    break;
                }
            }
        }
        if (use_audio) {
            adec = new kopaw::AudioDecoderNode();
            if (!adec->open(demux->audio_codec_params(), &err)) {
                KOP_LOG_ERROR(kTag, "音频解码器: %s", err.c_str());
                exit_code = 2;
                break;
            }
            if (!audio_filter_desc.empty()) {
                afilter = new kopaw::AudioFilterNode();
                if (!afilter->open(audio_filter_desc, &err)) {
                    KOP_LOG_ERROR(kTag, "音频滤镜节点: %s", err.c_str());
                    exit_code = 2;
                    break;
                }
            }
            asink = new kopaw::AudioSinkNode(g);  // g 在入图阶段回填
            if (!asink->open(&err)) {
                KOP_LOG_ERROR(kTag, "%s", err.c_str());
                exit_code = 2;
                break;
            }
        }

        // P2 硬解零拷贝：原生 DMA-BUF 输出对 Vulkan 本地渲染直连或 KOPMS
        // BUS 直通开放（滤镜/插件节点消费 CPU 帧，排除）。导出不可用时
        // 解码节点逐路径自动回退系统内存输出，此处不做硬性约束。
        if (vdec) {
#if KOPAW_HAVE_KOPMS_SINK
            const bool sink_direct = kopms_mode;
#else
            const bool sink_direct = false;
#endif
            bool native = (backend != nullptr || sink_direct) &&
                          vfilter == nullptr && use_node.empty();
            if (backend != nullptr && backend_name != "vulkan") native = false;
            if (zero_copy_mode == "off") {
                native = false;
            } else if (zero_copy_mode == "on" && !native) {
                KOP_LOG_WARN(kTag, "--zero-copy on 需要 Vulkan 渲染或 KOPMS 直通，已忽略");
                native = false;
            }
            vdec->set_native_output(native);
            if (native) {
                KOP_LOG_INFO(kTag,
                             "零拷贝：解码表面原生 DMA-BUF 直通下游"
                             "（VAAPI 导出失败回退；导入失败报告错误）");
            }
        }
        ready = true;
    } while (false);

    // ---- 阶段二：入图、连线、启动（此后节点只能经 graph_free 释放） ----
    uint32_t vrender_id = 0;
    uint32_t ins_id = 0;
    kopaw::RenderNode* vrender = nullptr;
    // 插件注册表必须与图同生命周期：dlclose 会卸载仍在使用的节点代码
    kopaw::PluginRegistry registry;
    if (ready) {
        g = kopaw_graph_new();
        kopaw_graph_set_event_cb(g, event_cb, nullptr);

        // P2 调度模式：KOPAW_SCHED=pool → 响应式节点走共享工作窃取线程池
        {
            const char* sched = getenv("KOPAW_SCHED");
            if (sched && strcmp(sched, "pool") == 0) {
                unsigned n = std::thread::hardware_concurrency();
                if (n == 0) n = 4;
                if (n > 8) n = 8;
                kopaw_graph_set_workers(g, n);
                KOP_LOG_INFO(kTag, "池调度：%u workers", n);
            }
        }

        KopawNodeDesc d_demux = demux->desc(g);
        uint32_t demux_id = kopaw_graph_add_node(g, &d_demux);
        uint32_t vdec_id = 0, adec_id = 0, asink_id = 0;
        bool wired = demux_id != 0;
        uint32_t vfilter_id = 0;
        uint32_t afilter_id = 0;
        if (vdec) {
            KopawNodeDesc d_vdec = vdec->desc(g);
            vdec_id = kopaw_graph_add_node(g, &d_vdec);
            vdec->set_output(kopaw_graph_node_output(g, vdec_id, 0));

            if (vfilter) {
                KopawNodeDesc d_vf = vfilter->desc(g);
                vfilter_id = kopaw_graph_add_node(g, &d_vf);
                vfilter->set_output(kopaw_graph_node_output(g, vfilter_id, 0));
                if (vfilter_id == 0 ||
                    kopaw_graph_connect(g, kopaw_graph_node_output(g, vdec_id, 0),
                                        vfilter_id, 0, 0) != KOPAW_OK) {
                    KOP_LOG_ERROR(kTag, "滤镜节点接线失败");
                    wired = false;
                }
            }

#if KOPAW_HAVE_KOPMS_SINK
            if (kopms_sink) {
                KopawNodeDesc d_sink = kopms_sink->desc();
                vrender_id = kopaw_graph_add_node(g, &d_sink);
                if (vrender_id == 0) {
                    KOP_LOG_ERROR(kTag, "KOPMS sink 节点注册失败");
                    wired = false;
                } else {
                    kopms_sink->set_graph(g, vrender_id);
                }
            } else
#endif
            {
                vrender = new kopaw::RenderNode(g, win, backend);
                KopawNodeDesc d_render = vrender->desc();
                vrender_id = kopaw_graph_add_node(g, &d_render);
                vrender->set_node_id(vrender_id);
            }
        }
        if (adec) {
            KopawNodeDesc d_adec = adec->desc(g);
            adec_id = kopaw_graph_add_node(g, &d_adec);
            adec->set_output(kopaw_graph_node_output(g, adec_id, 0));

            if (afilter) {
                KopawNodeDesc d_af = afilter->desc(g);
                afilter_id = kopaw_graph_add_node(g, &d_af);
                afilter->set_output(kopaw_graph_node_output(g, afilter_id, 0));
                if (afilter_id == 0) {
                    KOP_LOG_ERROR(kTag, "音频滤镜节点注册失败");
                    wired = false;
                }
            }

            KopawNodeDesc d_asink = asink->desc();
            asink_id = kopaw_graph_add_node(g, &d_asink);
            // P1 延迟记账：sink 需要 id 在 ring 排空后自行记账
            asink->set_graph(g);   // 构造时图未创建，入图后回填
            asink->set_node_id(asink_id);
        }
        demux->set_outputs(kopaw_graph_node_output(g, demux_id, 0),
                           kopaw_graph_node_output(g, demux_id, 1));

        const bool use_video2 = vdec != nullptr;
        if (use_video2) {
            // 源 → 视频解码
            if (kopaw_graph_connect(g, kopaw_graph_node_output(g, demux_id, 0), vdec_id,
                                    0, 0) != KOPAW_OK) {
                wired = false;
            }
            // P2.1 插件：先加载所有 --plugin，再按 --use 插入视频链。
            for (const auto& pp : plugin_paths) {
                std::string perr;
                if (!registry.load(pp, &perr)) {
                    KOP_LOG_ERROR(kTag, "插件加载失败 (%s): %s", pp.c_str(), perr.c_str());
                    wired = false;
                }
            }
            uint32_t tail_id = vdec_id;
            if (!use_node.empty()) {
                bool plugin_bound = false;
                uint32_t idx = 0;
                KopawNodeDesc d_ins{};
                const kopaw::LoadedPlugin* lp = registry.find_node(use_node, &idx);
                if (!lp) {
                    KOP_LOG_ERROR(kTag, "插件节点未找到: %s", use_node.c_str());
                    exit_code = 2;
                    wired = false;
                } else if (!registry.create_node(lp, idx, g, &d_ins, &err)) {
                    KOP_LOG_ERROR(kTag, "插件节点实例化失败 (%s): %s", use_node.c_str(),
                                  err.c_str());
                    exit_code = 2;
                    wired = false;
                } else if ((ins_id = kopaw_graph_add_node(g, &d_ins)) == 0) {
                    KOP_LOG_ERROR(kTag, "插件节点注册失败: %s", use_node.c_str());
                    if (d_ins.vtable && d_ins.vtable->destroy)
                        d_ins.vtable->destroy(d_ins.user_data);
                    exit_code = 2;
                    wired = false;
                }
                if (lp && ins_id != 0) {
                    if (!registry.bind_node_outputs(&d_ins, g, ins_id, &err)) {
                        KOP_LOG_ERROR(kTag, "插件输出绑定失败 (%s): %s", use_node.c_str(),
                                      err.c_str());
                        exit_code = 2;
                        wired = false;
                    } else {
                        plugin_bound = true;
                    }
                    if (plugin_bound) {
                        KOP_LOG_INFO(kTag, "已插入插件节点 %s（来自 %s）", use_node.c_str(),
                                     lp->name.c_str());
                        uint32_t prev = vfilter_id ? vfilter_id : vdec_id;
                        if (kopaw_graph_connect(g, kopaw_graph_node_output(g, prev, 0),
                                                ins_id, 0, 0) != KOPAW_OK ||
                            kopaw_graph_connect(g, kopaw_graph_node_output(g, ins_id, 0),
                                                vrender_id, 0, 0) != KOPAW_OK) {
                            wired = false;
                        }
                    }
                }
                tail_id = 0;  // 链路已替换
            }
            if (tail_id != 0) {
                // 无插件时：vdec → (filter) → render
                uint32_t prev = vfilter_id ? vfilter_id : vdec_id;
                if (kopaw_graph_connect(g, kopaw_graph_node_output(g, prev, 0), vrender_id,
                                        0, 0) != KOPAW_OK) {
                    wired = false;
                }
            }
        }
        if (adec) {
            if (kopaw_graph_connect(g, kopaw_graph_node_output(g, demux_id, 1), adec_id,
                                    0, 0) != KOPAW_OK ||
                (afilter_id != 0 &&
                 kopaw_graph_connect(g, kopaw_graph_node_output(g, adec_id, 0),
                                     afilter_id, 0, 0) != KOPAW_OK) ||
                kopaw_graph_connect(g,
                                    kopaw_graph_node_output(g, afilter_id != 0 ? afilter_id
                                                                               : adec_id,
                                                           0),
                                    asink_id, 0, 0) != KOPAW_OK) {
                wired = false;
            }
        }

        if (!wired || kopaw_graph_start(g) != KOPAW_OK) {
            KOP_LOG_ERROR(kTag, "图启动失败");
            exit_code = 2;
        } else {
            const bool use_video = vdec != nullptr;
            const bool use_audio = adec != nullptr;
#if KOPAW_HAVE_KOPMS_SINK
            const char* video_sink =
                kopms_sink ? "kopms-bus(零拷贝)" : (use_video ? backend->name() : "无");
#else
            const char* video_sink = use_video ? backend->name() : "无";
#endif
            KOP_LOG_INFO(kTag, "播放: %s（视频:%s 音频:%s 后端:%s）", file.c_str(),
                         use_video ? "开" : "关", use_audio ? "开" : "关",
                         use_video ? video_sink : "无");

            // ---- 主循环 ----
            const int64_t start_wall = kop::steady_us();
            const double dur_limit = duration > 0 ? duration : 1e18;
            const bool periodic_stats = getenv("KOPAW_STATS");
            int64_t last_stats = start_wall;
            std::string stats_buf(4096, '\0');
            while (!g_finished && !g_error) {
                if (vrender && vrender->close_requested()) {
                    KOP_LOG_INFO(kTag, "窗口关闭，停止播放");
                    break;
                }
                if (use_audio && kopaw_graph_clock_active(g)) {
                    if (kopaw_graph_clock_get(g) >= dur_limit * 1e6) break;
                } else if (duration > 0 &&
                           (kop::steady_us() - start_wall) >= duration * 1e6) {
                    break;  // 无音频时按墙上时钟计
                }
                if (periodic_stats && kop::steady_us() - last_stats >= 5'000'000) {
                    last_stats = kop::steady_us();
                    if (kopaw_graph_stats_json(g, stats_buf.data(),
                                               static_cast<uint32_t>(stats_buf.size())) > 0) {
                        KOP_LOG_INFO(kTag, "stats: %s", stats_buf.c_str());
                    }
                }
                kop::sleep_us(50'000);
            }
            exit_code = g_error ? 1 : 0;
        }
    }

    // ---- 收尾 ----
    if (g) {
        kopaw_graph_stop(g, 2000);
        // 性能基线：图销毁前导出最终统计
        {
            std::string stats_buf(8192, '\0');
            int n = kopaw_graph_stats_json(g, stats_buf.data(),
                                           static_cast<uint32_t>(stats_buf.size()));
            if (n > 0) KOP_LOG_INFO(kTag, "最终统计: %s", stats_buf.c_str());
        }
        kopaw_graph_free(g);  // 逐一调用节点 destroy；节点对象在此全部析构
        g = nullptr;
    } else {
        // 尚未入图：手动清理阶段一的对象
#if KOPAW_HAVE_KOPMS_SINK
        delete kopms_sink;
#endif
        delete asink;
        delete afilter;
        delete adec;
        delete vfilter;
        delete vdec;
        delete backend;
        delete demux;
    }
    if (win) {
        glfwDestroyWindow(win);
        win = nullptr;
    }
    if (glfw_inited) glfwTerminate();
    if (pa_inited) Pa_Terminate();
    return exit_code;
}
