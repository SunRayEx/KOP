// BUS2LAYER control-plane state used by the KOPMS window manager.
//
// The D-Bus adapter (or another KOP control producer) translates its method
// calls into KopmsControlCommandPayload values. This model owns the protocol
// state transition rules; it does not depend on a particular D-Bus runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kopms_protocol.h"

namespace kopms {

class ControlState {
public:
    // Compositor-facing view of one window in the managed tree.
    struct WindowInfo {
        uint64_t id = 0;
        uint64_t parent = 0;
        uint64_t owner_session = 0;
        // Session whose native DMA-BUF frames feed this window (0 = none).
        uint64_t attached_session = 0;
        uint32_t ownership = KOPMS_OWNERSHIP_EXCLUSIVE;
        uint64_t generation = 0;
    };

    void register_session(uint64_t session_id);
    void remove_session(uint64_t session_id);

    // Semantic failures are returned in ack->status. false is reserved for a
    // malformed API call that cannot produce a meaningful ACK.
    bool apply(uint64_t session_id, const KopmsControlCommandPayload& command,
               const std::vector<uint8_t>& data, KopmsControlAckPayload* ack,
               std::string* error);

    // Current window tree plus focus/clipboard state, for scene composition.
    std::vector<WindowInfo> snapshot() const;
    size_t window_count() const { return windows_.size(); }
    uint64_t focused_window() const { return focused_window_; }
    uint64_t clipboard_owner() const { return clipboard_owner_; }

private:
    struct Window {
        uint64_t owner_session = 0;
        uint64_t attached_session = 0;
        uint64_t parent = 0;
        uint64_t generation = 0;
        uint32_t ownership = KOPMS_OWNERSHIP_EXCLUSIVE;
    };

    uint64_t next_generation();
    bool session_active(uint64_t session_id) const;
    bool owns_window(uint64_t session_id, uint64_t window_id) const;
    bool parent_would_cycle(uint64_t window_id, uint64_t parent_id) const;
    void set_status(KopmsControlAckPayload* ack, KopmsControlStatus status,
                    uint64_t generation = 0);

    std::unordered_set<uint64_t> sessions_;
    std::unordered_map<uint64_t, Window> windows_;
    uint64_t focused_window_ = 0;
    uint64_t clipboard_owner_ = 0;
    uint64_t generation_ = 0;
};

}  // namespace kopms
