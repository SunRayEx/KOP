// P3-M3/M4 协议与生命周期测试。
//
// 覆盖里程碑要求的异常路径：
//   - HELLO Handle/modifier/explicit-sync 能力协商（新增能力位）
//   - Vulkan external memory（KOPAW_MEMORY_VULKAN）帧受 Handle 能力门控
//   - malformed FD：fd_count 与元数据不匹配（多余未引用 fd）
//   - 重复 release：FRAME_RELEASE 只发一次，重复显式释放被拒绝
//   - 跨 session handle：未知 session 无法释放在飞帧
//   - fence 超时：不可信号 fence 的有界等待
//   - WINDOW_ATTACH：窗口与会话绑定进入控制树快照
//   - 客户端异常退出：粗暴断连后在飞帧被服务端清理且服务存活
//
// 全部单线程：手动泵 wl_event_loop 驱动 KOPMS-S，裸 seqpacket 连接收发。
#include <chrono>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "drm_buffer.hpp"
#include "frame_bridge.h"
#include "kopms_server.h"
#include "wayland-server-core.h"

namespace {

bool fail(const char* message) {
    std::fprintf(stderr, "KOPMS M3/M4 test: %s\n", message);
    return false;
}

bool dispatch_once(wl_event_loop* loop) {
    return loop && wl_event_loop_dispatch(loop, 0) >= 0;
}

bool dispatch_connection(wl_event_loop* loop) {
    return dispatch_once(loop) && dispatch_once(loop);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Lifetime {
    std::atomic<int> released{0};
};

void retain_noop(KopmsFrameDescriptor*) {}
void release_count(KopmsFrameDescriptor* frame) {
    static_cast<Lifetime*>(frame->user_data)->released.fetch_add(1);
}

KopmsFrameDescriptor make_descriptor(Lifetime* lifetime, int fd, uint32_t memory_type,
                                     uint64_t modifier, uint32_t fence_kind,
                                     int fence_fd) {
    KopmsFrameDescriptor frame{};
    frame.struct_size = sizeof(frame);
    frame.version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    frame.media_type = KOPAW_MEDIA_VIDEO;
    frame.width = 32;
    frame.height = 32;
    frame.format = 0x34324241u;  // AB24（KOPAW 导出格式）
    frame.memory_type = memory_type;
    frame.dma_buf_handle = static_cast<uint64_t>(static_cast<uint32_t>(fd));
    frame.plane_count = 1;
    frame.planes[0].fd = fd;
    frame.planes[0].stride = 256;
    frame.planes[0].modifier = modifier;
    frame.acquire_fence.kind = fence_kind;
    frame.acquire_fence.fd = fence_fd;
    frame.user_data = lifetime;
    frame.retain = &retain_noop;
    frame.release = &release_count;
    return frame;
}

// 握手：发送 HELLO → 泵事件循环 → 收 HELLO_ACK。
bool hello_with(kopms::BusConnection* con, wl_event_loop* loop, uint64_t caps,
                KopmsHelloAckPayload* ack, std::string* error) {
    KopmsHelloPayload hello{};
    hello.struct_size = KOPMS_HELLO_PAYLOAD_SIZE;
    hello.protocol_major = KOPMS_PROTOCOL_MAJOR;
    hello.protocol_minor = KOPMS_PROTOCOL_MINOR;
    hello.capabilities = caps;
    hello.max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    hello.max_fds = KOPMS_PROTOCOL_MAX_FDS;
    if (!con->send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                           KOPMS_MESSAGE_HELLO, KOPMS_MESSAGE_FLAG_CONTROL, 1,
                           kopms::encode_hello(hello), {}, error)) {
        return false;
    }
    if (!dispatch_connection(loop)) return false;
    kopms::BusMessage reply;
    if (con->receive(&reply, error) != kopms::BusReceiveStatus::Message ||
        reply.header.type != KOPMS_MESSAGE_HELLO_ACK) {
        if (error) *error = "HELLO_ACK missing";
        return false;
    }
    return kopms::decode_hello_ack(reply.payload, ack, error) && ack->status == 0;
}

// 发送控制命令并等待 ACK。
bool control_command(kopms::BusConnection* con, wl_event_loop* loop, uint64_t sequence,
                     const KopmsControlCommandPayload& command,
                     KopmsControlAckPayload* ack, std::string* error) {
    if (!con->send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                           KOPMS_MESSAGE_CONTROL_COMMAND,
                           KOPMS_MESSAGE_FLAG_CONTROL, sequence,
                           kopms::encode_control_command(command, {}), {}, error)) {
        return false;
    }
    if (!dispatch_connection(loop)) return false;
    kopms::BusMessage reply;
    if (con->receive(&reply, error) != kopms::BusReceiveStatus::Message ||
        reply.header.type != KOPMS_MESSAGE_CONTROL_ACK) {
        if (error) *error = "CONTROL_ACK missing";
        return false;
    }
    if (!kopms::decode_control_ack(reply.payload, ack, error)) return false;
    return ack->request_sequence == sequence;
}

// 提交 descriptor（媒体 lane）并泵一轮服务端。
bool submit_frame(kopms::BusConnection* con, wl_event_loop* loop, uint64_t sequence,
                  const KopmsFrameDescriptor& desc, uint32_t frame_id,
                  std::string* error) {
    KopmsFrameSubmitPayload payload{};
    std::vector<int> fds;
    if (!kopms::make_frame_submit(&desc, frame_id, &payload, &fds, error)) return false;
    if (!con->send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                           KOPMS_MESSAGE_FRAME_SUBMIT, KOPMS_MESSAGE_FLAG_MEDIA,
                           sequence, kopms::encode_frame_submit(payload), fds,
                           error)) {
        return false;
    }
    return dispatch_connection(loop);
}

// 等待指定 frame 的 FRAME_RELEASE（泵循环，有界）。
bool wait_release(kopms::BusConnection* con, wl_event_loop* loop, uint32_t frame_id,
                  KopmsFrameReleasePayload* release, std::string* error) {
    const int64_t deadline = now_ms() + 2000;
    for (;;) {
        kopms::BusMessage message;
        const kopms::BusReceiveStatus status = con->receive(&message, error);
        if (status == kopms::BusReceiveStatus::Message) {
            if (message.header.type == KOPMS_MESSAGE_FRAME_RELEASE &&
                kopms::decode_frame_release(message.payload, release, error) &&
                release->frame_id == frame_id) {
                return true;
            }
            continue;
        }
        if (status != kopms::BusReceiveStatus::WouldBlock) return false;
        if (!dispatch_once(loop)) return false;
        if (now_ms() > deadline) {
            if (error) *error = "FRAME_RELEASE timeout";
            return false;
        }
    }
}

bool run() {
    const std::string socket_path =
        "/tmp/kopms-m34-test-" + std::to_string(static_cast<long long>(getpid()));
    wl_event_loop* loop = wl_event_loop_create();
    if (!loop) return fail("cannot create event loop");

    std::atomic<int> submitted{0};
    uint64_t producer_session = 0;
    kopms::KopmsServer server(
        loop, socket_path,
        [&](kopms::KopmsReceivedFrame& frame) {
            ++submitted;
            producer_session = frame.session_id;
            return kopms::FrameDisposition::Retain;  // 保留到显式释放
        },
        kopms::GpuMode::Hybrid);
    std::string error;
    if (!server.start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return fail("server start");
    }

    // ---- 1. HELLO 能力协商：Handle/modifier/explicit-sync 位必须回显 ----
    {
        int fd = -1;
        if (!kopms::connect_unix_seqpacket(socket_path, &fd, &error))
            return fail("negotiation connect");
        kopms::BusConnection client(fd);
        KopmsHelloAckPayload ack{};
        const uint64_t caps = KOPMS_PROTOCOL_CAP_DMABUF |
                              KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                              KOPMS_PROTOCOL_CAP_HANDLE_FRAMES |
                              KOPMS_PROTOCOL_CAP_MODIFIERS |
                              KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC;
        if (!hello_with(&client, loop, caps, &ack, &error)) {
            return fail(("negotiation hello: " + error).c_str());
        }
        if ((ack.capabilities & KOPMS_PROTOCOL_CAP_HANDLE_FRAMES) == 0 ||
            (ack.capabilities & KOPMS_PROTOCOL_CAP_MODIFIERS) == 0 ||
            (ack.capabilities & KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC) == 0) {
            return fail("new P3 capability bits were not negotiated");
        }
        if (ack.selected_minor < 1) return fail("protocol minor < 1");
        client.close();
        dispatch_once(loop);
    }

    // ---- 2. 能力门控：未协商 Handle 的会话提交 Vulkan 外部内存帧被拒 ----
    {
        int fd = -1;
        if (!kopms::connect_unix_seqpacket(socket_path, &fd, &error))
            return fail("gated connect");
        kopms::BusConnection gated(fd);
        KopmsHelloAckPayload ack{};
        if (!hello_with(&gated, loop,
                        KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE,
                        &ack, &error)) {
            return fail("gated hello");
        }
        if ((ack.capabilities & KOPMS_PROTOCOL_CAP_HANDLE_FRAMES) != 0) {
            return fail("server must not grant Handle bits the client didn't ask for");
        }
        const int frame_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        Lifetime lifetime;
        KopmsFrameDescriptor desc = make_descriptor(
            &lifetime, frame_fd, KOPAW_MEMORY_VULKAN, 0, KOPAW_SYNC_FENCE_NONE, -1);
        if (!submit_frame(&gated, loop, 2, desc, 1, &error)) return fail("gated submit");
        kopms::BusMessage err_msg;
        if (gated.receive(&err_msg, &error) != kopms::BusReceiveStatus::Message ||
            err_msg.header.type != KOPMS_MESSAGE_ERROR) {
            return fail("Vulkan frame without Handle cap was not rejected");
        }
        gated.close();
        ::close(frame_fd);
        dispatch_once(loop);
    }

    // ---- 3. Vulkan 帧 + fence 提交、重复 release、跨 session handle ----
    int fd = -1;
    if (!kopms::connect_unix_seqpacket(socket_path, &fd, &error))
        return fail("producer connect");
    kopms::BusConnection producer(fd);
    KopmsHelloAckPayload ack{};
    if (!hello_with(&producer, loop,
                    KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                        KOPMS_PROTOCOL_CAP_HANDLE_FRAMES |
                        KOPMS_PROTOCOL_CAP_MODIFIERS |
                        KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC |
                        KOPMS_PROTOCOL_CAP_CONTROL_STATE,
                    &ack, &error)) {
        return fail(("producer hello: " + error).c_str());
    }

    const int frame_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    if (frame_fd < 0) return fail("open frame fd");
    Lifetime lifetime;
    KopmsFrameDescriptor desc = make_descriptor(&lifetime, frame_fd,
                                                KOPAW_MEMORY_VULKAN, 0 /*LINEAR*/,
                                                KOPAW_SYNC_FENCE_NONE, -1);
    if (!submit_frame(&producer, loop, 2, desc, 1, &error)) {
        std::fprintf(stderr, "vulkan submit: %s\n", error.c_str());
        return fail("vulkan frame submit");
    }
    if (submitted.load() != 1) return fail("retained submit was not dispatched");
    if (producer_session == 0) return fail("handler did not observe session");

    // 跨 session handle：未知 session 无法释放在飞帧。
    if (server.release_frame(producer_session + 9999, 1)) {
        return fail("cross-session release must fail");
    }
    // 重复 release：同一 frame_id 只能显式释放一次。
    if (!server.release_frame(producer_session, 1)) return fail("release #1");
    if (server.release_frame(producer_session, 1)) {
        return fail("duplicate release_frame must fail");
    }
    KopmsFrameReleasePayload release{};
    if (!wait_release(&producer, loop, 1, &release, &error)) {
        std::fprintf(stderr, "wait release: %s\n", error.c_str());
        return fail("FRAME_RELEASE not received");
    }
    if (release.status != KOPMS_FRAME_RELEASE_OK) return fail("release status invalid");

    // ---- 4. WINDOW_ATTACH：窗口与会话绑定进入控制树快照 ----
    {
        KopmsControlCommandPayload command{};
        command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
        command.operation = KOPMS_CONTROL_WINDOW_CREATE;
        command.object_id = 4242;
        KopmsControlAckPayload control_ack{};
        if (!control_command(&producer, loop, 3, command, &control_ack, &error) ||
            control_ack.status != KOPMS_CONTROL_STATUS_OK) {
            return fail("window create");
        }
        command.operation = KOPMS_CONTROL_WINDOW_ATTACH;
        command.value = 1;
        if (!control_command(&producer, loop, 4, command, &control_ack, &error) ||
            control_ack.status != KOPMS_CONTROL_STATUS_OK) {
            return fail("window attach");
        }
        bool bound = false;
        for (const auto& info : server.control_snapshot()) {
            if (info.id == 4242 && info.attached_session == producer_session) {
                bound = true;
            }
        }
        if (!bound) return fail("attach not visible in snapshot");
    }

    // ---- 5. fence 超时：不可信号 fd 的有界等待（importer 侧原语） ----
    {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) return fail("pipe");
        kopms::DrmDmabufImportRequest request{};
        request.acquire_fence_kind = KOPAW_SYNC_FENCE_FD;
        request.acquire_fence_fd = pipe_fds[0];  // 无数据，永不 POLLIN
        std::string fence_error;
        const bool timed_out =
            !kopms::wait_for_acquire_fence(request, 60, &fence_error);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        if (!timed_out) return fail("unsignaled fence did not time out");
    }

    // ---- 6. malformed FD：未引用 fd（fd_count 与元数据不匹配）被拒 ----
    {
        const int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        Lifetime lf;
        KopmsFrameDescriptor d = make_descriptor(&lf, null_fd, KOPAW_MEMORY_DMABUF, 0,
                                                 KOPAW_SYNC_FENCE_NONE, -1);
        KopmsFrameSubmitPayload payload{};
        std::vector<int> fds;
        if (!kopms::make_frame_submit(&d, 77, &payload, &fds, &error))
            return fail("encode malformed frame");
        ::close(null_fd);
        fds.push_back(dup(fds[0]));  // 多余未引用 fd
        std::string validate_error;
        if (kopms::validate_frame_submit_caps(payload, fds.size(), 0xffffffffull,
                                              &validate_error)) {
            return fail("unreferenced extra fd was accepted");
        }
        ::close(fds.back());
    }

    // ---- 7. 客户端异常退出：粗暴断连后在飞帧被服务端清理 ----
    {
        int abrupt_fd = -1;
        if (!kopms::connect_unix_seqpacket(socket_path, &abrupt_fd, &error))
            return fail("abrupt connect");
        kopms::BusConnection abrupt(abrupt_fd);
        KopmsHelloAckPayload abrupt_ack{};
        const uint64_t caps = KOPMS_PROTOCOL_CAP_DMABUF |
                              KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                              KOPMS_PROTOCOL_CAP_HANDLE_FRAMES;
        if (!hello_with(&abrupt, loop, caps, &abrupt_ack, &error))
            return fail("abrupt hello");
        const int frame_fd2 = open("/dev/zero", O_RDONLY | O_CLOEXEC);
        Lifetime lf;
        KopmsFrameDescriptor d = make_descriptor(&lf, frame_fd2, KOPAW_MEMORY_VULKAN,
                                                 0, KOPAW_SYNC_FENCE_NONE, -1);
        if (!submit_frame(&abrupt, loop, 2, d, 9, &error)) return fail("abrupt submit");
        ::close(frame_fd2);
        // 不发 GOODBYE，直接关 socket —— 服务端必须在下一轮清理在飞帧与 fd。
        abrupt.close();
        for (int i = 0; i < 8; ++i) dispatch_once(loop);
        // frame 9 属于 abrupt 会话；其会话移除后必须不可再释放。
        if (server.release_frame(producer_session, 9)) {
            return fail("abrupt session frame survived disconnect");
        }
    }

    // 服务端必须仍能服务新会话（断连清理没有破坏状态）。
    {
        int after_fd = -1;
        if (!kopms::connect_unix_seqpacket(socket_path, &after_fd, &error))
            return fail("post-disconnect connect");
        kopms::BusConnection after(after_fd);
        KopmsHelloAckPayload after_ack{};
        if (!hello_with(&after, loop, KOPMS_PROTOCOL_CAP_CONTROL_STATE, &after_ack,
                        &error)) {
            return fail(("post-disconnect hello: " + error).c_str());
        }
        after.close();
        dispatch_once(loop);
    }

    producer.close();
    server.stop();
    return true;
}

}  // namespace

int main() { return run() ? 0 : 1; }
