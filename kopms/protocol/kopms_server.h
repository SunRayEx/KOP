// KOPMS-S session server. Wayland and KOPMS sessions share the compositor's
// wl_event_loop, while BUS2LAYER owns packet and FD transport details.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "bus2layer.h"
#include "control_state.h"
#include "gpu_mode.hpp"

struct wl_event_loop;
struct wl_event_source;

namespace kopms {

struct KopmsReceivedFrame {
    KopmsReceivedFrame() = default;
    ~KopmsReceivedFrame();

    KopmsReceivedFrame(const KopmsReceivedFrame&) = delete;
    KopmsReceivedFrame& operator=(const KopmsReceivedFrame&) = delete;
    KopmsReceivedFrame(KopmsReceivedFrame&& other) noexcept;
    KopmsReceivedFrame& operator=(KopmsReceivedFrame&& other) noexcept;

    uint64_t session_id = 0;
    // A process-local handle materialized from fds after SCM_RIGHTS receive.
    // It remains valid only while this object retains the corresponding fd.
    uint64_t dma_buf_handle = 0;
    GpuMode gpu_mode = GpuMode::Single;
    KopmsFrameSubmitPayload payload{};
    std::vector<int> fds;
};

enum class FrameDisposition {
    Release,
    Retain,
    // 回压拒收：立即以 KOPMS_FRAME_RELEASE_DROPPED 释放（M4 帧回压信号）。
    ReleaseDropped,
    // 导入/契约失败：生产者应关闭原生路径并回退其 CPU 路径。
    ReleaseRejected,
};

using FrameHandler = std::function<FrameDisposition(KopmsReceivedFrame& frame)>;

class KopmsServer {
public:
    KopmsServer(wl_event_loop* loop, std::string socket_name,
                FrameHandler frame_handler = {},
                GpuMode gpu_mode = GpuMode::Single);
    ~KopmsServer();

    KopmsServer(const KopmsServer&) = delete;
    KopmsServer& operator=(const KopmsServer&) = delete;

    bool start(std::string* error);
    void stop();
    bool running() const { return listener_fd_ >= 0; }
    const std::string& socket_path() const { return socket_path_; }
    GpuMode gpu_mode() const { return gpu_mode_; }

    // Replace the frame disposition handler (event-loop thread only).
    void set_frame_handler(FrameHandler handler) { frame_handler_ = std::move(handler); }

    // Control-plane snapshot for scene layout (event-loop thread only).
    std::vector<ControlState::WindowInfo> control_snapshot() const;
    uint64_t focused_control_window() const;

    // Must be called from the compositor event-loop thread. It sends the
    // release event and closes the server-owned DMA-BUF/fence references.
    bool release_frame(uint64_t session_id, uint32_t frame_id,
                       uint32_t status = KOPMS_FRAME_RELEASE_OK);

private:
    struct Session;

    static int listener_event(int fd, uint32_t mask, void* data);
    static int session_event(int fd, uint32_t mask, void* data);

    int accept_clients();
    int process_session(Session* session, uint32_t mask);
    bool handle_message(Session* session, BusMessage&& message);
    bool send_error(Session* session, KopmsErrorCode code, uint16_t type,
                    const std::string& message);
    bool send_release(Session* session, uint32_t frame_id, uint32_t status);
    bool send_control_ack(Session* session, const KopmsControlAckPayload& ack);
    void remove_session(Session* session);

    wl_event_loop* loop_ = nullptr;
    std::string socket_name_;
    std::string socket_path_;
    int listener_fd_ = -1;
    wl_event_source* listener_source_ = nullptr;
    uint64_t next_session_id_ = 1;
    uint64_t next_server_sequence_ = 1;
    FrameHandler frame_handler_;
    GpuMode gpu_mode_ = GpuMode::Single;
    ControlState control_state_;
    std::list<std::unique_ptr<Session>> sessions_;
};

}  // namespace kopms
