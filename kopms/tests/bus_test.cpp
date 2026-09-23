#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "frame_bridge.h"
#include "kopms_client.h"
#include "kopms_server.h"
#include "wayland-server-core.h"

namespace {

struct Lifetime {
    std::atomic<int> retained{0};
    std::atomic<int> released{0};
};

void retain_frame(KopmsFrameDescriptor* frame) {
    static_cast<Lifetime*>(frame->user_data)->retained.fetch_add(1);
}

void release_frame(KopmsFrameDescriptor* frame) {
    static_cast<Lifetime*>(frame->user_data)->released.fetch_add(1);
}

bool fail(const char* message) {
    std::fprintf(stderr, "KOPMS BUS test: %s\n", message);
    return false;
}

bool dispatch_once(wl_event_loop* loop) {
    return loop && wl_event_loop_dispatch(loop, 0) >= 0;
}

bool dispatch_connection(wl_event_loop* loop) {
    // The first dispatch accepts the Unix socket; the next dispatch services
    // the newly registered session source.
    return dispatch_once(loop) && dispatch_once(loop);
}

bool send_raw_header(kopms::BusConnection* connection, uint16_t major,
                     uint32_t payload_size, uint32_t fd_count) {
    if (!connection) return false;
    KopmsMessageHeader header{};
    header.magic = KOPMS_PROTOCOL_MAGIC;
    header.major = major;
    header.minor = KOPMS_PROTOCOL_MINOR;
    header.type = KOPMS_MESSAGE_HELLO;
    header.sequence = 1;
    header.payload_size = payload_size;
    header.fd_count = fd_count;
    kopms::WireHeader wire{};
    kopms::encode_message_header(header, &wire);
    iovec vector{};
    vector.iov_base = wire.data();
    vector.iov_len = wire.size();
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    return sendmsg(connection->fd(), &message, MSG_NOSIGNAL) ==
           static_cast<ssize_t>(wire.size());
}

bool make_test_frame(Lifetime* lifetime, int fd, KopmsFrameDescriptor* frame) {
    if (!lifetime || !frame) return false;
    *frame = {};
    frame->struct_size = sizeof(*frame);
    frame->version = KOPMS_FRAME_DESCRIPTOR_VERSION;
    frame->media_type = KOPAW_MEDIA_VIDEO;
    frame->width = 64;
    frame->height = 64;
    frame->format = 0x34325241;
    frame->memory_type = KOPAW_MEMORY_DMABUF;
    frame->dma_buf_handle = static_cast<uint64_t>(fd);
    frame->plane_count = 1;
    frame->planes[0].fd = fd;
    frame->planes[0].stride = 256;
    frame->acquire_fence.kind = KOPAW_SYNC_FENCE_NONE;
    frame->acquire_fence.fd = -1;
    frame->user_data = lifetime;
    frame->retain = retain_frame;
    frame->release = release_frame;
    return true;
}

bool send_control_and_receive(kopms::BusConnection* client, wl_event_loop* loop,
                              uint64_t sequence,
                              const KopmsControlCommandPayload& command,
                              KopmsControlAckPayload* ack, std::string* error) {
    if (!client || !loop || !ack ||
        !client->send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                              KOPMS_MESSAGE_CONTROL_COMMAND,
                              KOPMS_MESSAGE_FLAG_CONTROL, sequence,
                              kopms::encode_control_command(command, {}), {}, error) ||
        !dispatch_once(loop)) {
        return false;
    }
    kopms::BusMessage message;
    if (client->receive(&message, error) != kopms::BusReceiveStatus::Message ||
        message.header.type != KOPMS_MESSAGE_CONTROL_ACK ||
        message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                  KOPMS_MESSAGE_FLAG_CONTROL) ||
        !message.fds.empty() ||
        !kopms::decode_control_ack(message.payload, ack, error) ||
        ack->request_sequence != sequence) {
        return false;
    }
    return true;
}

bool run() {
    const std::string socket_path =
        "/tmp/kopms-bus-test-" + std::to_string(static_cast<long long>(getpid()));
    wl_event_loop* loop = wl_event_loop_create();
    if (!loop) return fail("cannot create Wayland event loop");

    int handled_frames = 0;
    uint64_t retained_session = 0;
    uint64_t received_handle = 0;
    kopms::GpuMode received_mode = kopms::GpuMode::Single;
    bool received_handle_matches_fd = false;
    kopms::KopmsServer server(
        loop, socket_path,
        [&handled_frames, &retained_session, &received_handle,
         &received_mode, &received_handle_matches_fd](kopms::KopmsReceivedFrame& frame) {
            if (frame.payload.media_type != KOPAW_MEDIA_VIDEO || frame.fds.size() != 1) {
                return kopms::FrameDisposition::Release;
            }
            ++handled_frames;
            received_handle = frame.dma_buf_handle;
            received_mode = frame.gpu_mode;
            received_handle_matches_fd =
                frame.dma_buf_handle == static_cast<uint64_t>(frame.fds[0]);
            if (!received_handle_matches_fd) {
                return kopms::FrameDisposition::Release;
            }
            if (frame.payload.frame_id == 8) {
                retained_session = frame.session_id;
                return kopms::FrameDisposition::Retain;
            }
            return kopms::FrameDisposition::Release;
        },
        kopms::GpuMode::Hybrid);
    std::string error;
    if (!server.start(&error)) {
        wl_event_loop_destroy(loop);
        std::fprintf(stderr, "KOPMS BUS test: start failed: %s\n", error.c_str());
        return false;
    }

    // A valid HELLO establishes the KOPMS-S/KOPMS-C session.
    int client_fd = -1;
    if (!kopms::connect_unix_seqpacket(socket_path, &client_fd, &error)) {
        server.stop();
        wl_event_loop_destroy(loop);
        std::fprintf(stderr, "KOPMS BUS test: connect failed: %s\n", error.c_str());
        return false;
    }
    kopms::BusConnection client(client_fd);
    KopmsHelloPayload hello{};
    hello.struct_size = KOPMS_HELLO_PAYLOAD_SIZE;
    hello.protocol_major = KOPMS_PROTOCOL_MAJOR;
    hello.protocol_minor = KOPMS_PROTOCOL_MINOR;
    hello.capabilities = KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                         KOPMS_PROTOCOL_CAP_CONTROL_STATE |
                         KOPMS_PROTOCOL_CAP_COLOR_METADATA;
    hello.max_payload = KOPMS_PROTOCOL_MAX_PAYLOAD;
    hello.max_fds = KOPMS_PROTOCOL_MAX_FDS;
    if (!client.send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                             KOPMS_MESSAGE_HELLO, KOPMS_MESSAGE_FLAG_CONTROL, 1,
                             kopms::encode_hello(hello), {}, &error) ||
        !dispatch_connection(loop)) {
        return fail("valid HELLO dispatch failed");
    }
    kopms::BusMessage message;
    if (client.receive(&message, &error) != kopms::BusReceiveStatus::Message ||
        message.header.type != KOPMS_MESSAGE_HELLO_ACK ||
        message.header.flags != (KOPMS_MESSAGE_FLAG_REPLY |
                                  KOPMS_MESSAGE_FLAG_CONTROL)) {
        return fail("HELLO_ACK was not received");
    }
    KopmsHelloAckPayload ack{};
    if (!kopms::decode_hello_ack(message.payload, &ack, &error) || ack.status != 0 ||
        (ack.capabilities & KOPMS_PROTOCOL_CAP_DMABUF) == 0 ||
        (ack.capabilities & KOPMS_PROTOCOL_CAP_CONTROL_STATE) == 0 ||
        (ack.capabilities & KOPMS_PROTOCOL_CAP_COLOR_METADATA) == 0) {
        return fail("HELLO_ACK negotiation is invalid");
    }
    message.clear();

    // CONTROL commands update the window-manager state without touching the
    // MEDIA lane. Parent cycles and destruction with live children are
    // rejected while focus, clipboard, and ownership changes are acknowledged.
    KopmsControlCommandPayload command{};
    KopmsControlAckPayload control_ack{};
    command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
    command.operation = KOPMS_CONTROL_WINDOW_CREATE;
    command.object_id = 100;
    if (!send_control_and_receive(&client, loop, 2, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK || control_ack.generation == 0) {
        return fail("root window creation failed");
    }
    command.object_id = 101;
    command.related_id = 100;
    if (!send_control_and_receive(&client, loop, 3, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("child window creation failed");
    }
    command.operation = KOPMS_CONTROL_WINDOW_SET_PARENT;
    command.object_id = 100;
    command.related_id = 101;
    if (!send_control_and_receive(&client, loop, 4, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_CONFLICT) {
        return fail("parent cycle was not rejected");
    }
    command.operation = KOPMS_CONTROL_FOCUS_SET;
    command.object_id = 101;
    command.related_id = 0;
    if (!send_control_and_receive(&client, loop, 5, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("focus update failed");
    }
    command.operation = KOPMS_CONTROL_CLIPBOARD_SET_OWNER;
    if (!send_control_and_receive(&client, loop, 6, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("clipboard ownership update failed");
    }
    command.operation = KOPMS_CONTROL_OWNERSHIP_SET;
    command.value = KOPMS_OWNERSHIP_SHARED;
    if (!send_control_and_receive(&client, loop, 7, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("window ownership update failed");
    }
    command.operation = KOPMS_CONTROL_WINDOW_DESTROY;
    command.object_id = 100;
    command.value = 0;
    if (!send_control_and_receive(&client, loop, 8, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_CONFLICT) {
        return fail("parent destroy with a live child was not rejected");
    }
    command.object_id = 101;
    command.related_id = 0;
    if (!send_control_and_receive(&client, loop, 9, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("child window destruction failed");
    }
    command.object_id = 100;
    if (!send_control_and_receive(&client, loop, 10, command, &control_ack, &error) ||
        control_ack.status != KOPMS_CONTROL_STATUS_OK) {
        return fail("root window destruction failed");
    }

    // Transfer a real descriptor-shaped frame and verify the release message.
    const int frame_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (frame_fd < 0) return fail("cannot open test frame fd");
    Lifetime lifetime;
    KopmsFrameDescriptor frame{};
    make_test_frame(&lifetime, frame_fd, &frame);
    KopmsFrameSubmitPayload submit{};
    std::vector<int> fds;
    if (!kopms::make_frame_submit(&frame, 7, &submit, &fds, &error) || fds.size() != 1 ||
        !client.send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                             KOPMS_MESSAGE_FRAME_SUBMIT, KOPMS_MESSAGE_FLAG_MEDIA, 11,
                             kopms::encode_frame_submit(submit), fds, &error) ||
        !dispatch_connection(loop)) {
        ::close(frame_fd);
        return fail("FRAME_SUBMIT dispatch failed");
    }
    ::close(frame_fd);
    message.clear();
    if (client.receive(&message, &error) != kopms::BusReceiveStatus::Message ||
        message.header.type != KOPMS_MESSAGE_FRAME_RELEASE) {
        return fail("FRAME_RELEASE was not received");
    }
    KopmsFrameReleasePayload release{};
    if (!kopms::decode_frame_release(message.payload, &release, &error) ||
        release.frame_id != 7 || release.status != KOPMS_FRAME_RELEASE_OK ||
        handled_frames != 1 || !received_handle_matches_fd || received_handle == 0 ||
        received_mode != kopms::GpuMode::Hybrid) {
        return fail("FRAME_RELEASE contents are invalid");
    }

    // A retained server reference stays in flight until the compositor
    // explicitly returns it through the session API.
    const int retained_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (retained_fd < 0) return fail("cannot open retained test frame fd");
    Lifetime retained_lifetime;
    KopmsFrameDescriptor retained_frame{};
    make_test_frame(&retained_lifetime, retained_fd, &retained_frame);
    KopmsFrameSubmitPayload retained_submit{};
    std::vector<int> retained_fds;
    if (!kopms::make_frame_submit(&retained_frame, 8, &retained_submit,
                                  &retained_fds, &error) ||
        !client.send_message(KOPMS_PROTOCOL_MAJOR, KOPMS_PROTOCOL_MINOR,
                             KOPMS_MESSAGE_FRAME_SUBMIT, KOPMS_MESSAGE_FLAG_MEDIA, 12,
                             kopms::encode_frame_submit(retained_submit),
                             retained_fds, &error) ||
        !dispatch_once(loop)) {
        ::close(retained_fd);
        return fail("retained FRAME_SUBMIT dispatch failed");
    }
    ::close(retained_fd);
    if (retained_session == 0 || handled_frames != 2 ||
        client.wait_readable(0, &error) != kopms::BusWaitStatus::Timeout ||
        !server.release_frame(retained_session, 8)) {
        return fail("retained frame was not held for explicit release");
    }
    message.clear();
    if (client.receive(&message, &error) != kopms::BusReceiveStatus::Message ||
        !kopms::decode_frame_release(message.payload, &release, &error) ||
        release.frame_id != 8 || release.status != KOPMS_FRAME_RELEASE_OK) {
        return fail("explicit retained frame release is invalid");
    }

    // A major-version mismatch must be rejected by KOPMS-S.
    int bad_version_fd = -1;
    if (!kopms::connect_unix_seqpacket(socket_path, &bad_version_fd, &error)) {
        return fail("version test connect failed");
    }
    kopms::BusConnection bad_version(bad_version_fd);
    if (!bad_version.send_message(99, KOPMS_PROTOCOL_MINOR, KOPMS_MESSAGE_HELLO,
                                  KOPMS_MESSAGE_FLAG_CONTROL, 1,
                                  kopms::encode_hello(hello), {}, &error) ||
        !dispatch_connection(loop) ||
        bad_version.receive(&message, &error) != kopms::BusReceiveStatus::Message) {
        return fail("version mismatch was not dispatched");
    }
    KopmsErrorCode code = KOPMS_ERROR_MALFORMED;
    uint16_t offending_type = 0;
    std::string text;
    if (!kopms::decode_error(message.payload, &code, &offending_type, &text, &error) ||
        code != KOPMS_ERROR_VERSION || offending_type != KOPMS_MESSAGE_HELLO) {
        return fail("version mismatch error is invalid");
    }
    bad_version.close();

    // The BUS layer rejects a packet whose declared payload is longer than the
    // packet received, before the protocol layer can inspect it.
    int malformed_fd = -1;
    if (!kopms::connect_unix_seqpacket(socket_path, &malformed_fd, &error)) {
        return fail("malformed packet test connect failed");
    }
    kopms::BusConnection malformed(malformed_fd);
    if (!send_raw_header(&malformed, KOPMS_PROTOCOL_MAJOR, 1, 0) ||
        !dispatch_connection(loop)) {
        return fail("malformed packet was not dispatched");
    }
    const auto malformed_status = malformed.receive(&message, &error);
    if (malformed_status != kopms::BusReceiveStatus::Closed &&
        malformed_status != kopms::BusReceiveStatus::Error) {
        return fail("malformed packet was not closed");
    }
    malformed.close();

    // Exercise the public KOPMS-C API against the same server. The server
    // event loop runs independently here, as it does in the compositor.
    std::atomic<bool> stop_event_loop{false};
    std::thread event_thread([&] {
        while (!stop_event_loop.load()) {
            if (wl_event_loop_dispatch(loop, 10) < 0) break;
        }
    });
    kopms::KopmsClient api_client;
    std::string api_error;
    bool api_ok = api_client.connect(socket_path, &api_error) &&
                  api_client.hello(KOPMS_PROTOCOL_CAP_CONTROL_STATE, &api_error);
    uint64_t request_sequence = 0;
    KopmsControlAckPayload api_ack{};
    if (api_ok) {
        KopmsControlCommandPayload api_command{};
        api_command.struct_size = KOPMS_CONTROL_PAYLOAD_SIZE;
        api_command.operation = KOPMS_CONTROL_WINDOW_CREATE;
        api_command.object_id = 200;
        api_ok = api_client.send_control(api_command, {}, &request_sequence, &api_error) &&
                 api_client.wait_for_control_ack(request_sequence, 1000, &api_ack,
                                                 &api_error) &&
                 api_ack.status == KOPMS_CONTROL_STATUS_OK;
        if (api_ok) {
            api_command.operation = KOPMS_CONTROL_WINDOW_DESTROY;
            api_ok = api_client.send_control(api_command, {}, &request_sequence, &api_error) &&
                     api_client.wait_for_control_ack(request_sequence, 1000, &api_ack,
                                                     &api_error) &&
                     api_ack.status == KOPMS_CONTROL_STATUS_OK;
        }
    }
    api_client.disconnect();
    stop_event_loop.store(true);
    event_thread.join();
    if (!api_ok) {
        std::fprintf(stderr, "KOPMS BUS test: client API failed: %s\n",
                     api_error.c_str());
        return false;
    }

    client.close();
    server.stop();
    wl_event_loop_destroy(loop);
    return true;
}

}  // namespace

int main() { return run() ? 0 : 1; }
