#include "control_state.h"

#include <algorithm>

namespace kopms {

void ControlState::register_session(uint64_t session_id) {
    if (session_id != 0) sessions_.insert(session_id);
}

void ControlState::remove_session(uint64_t session_id) {
    if (session_id == 0) return;
    sessions_.erase(session_id);
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->second.owner_session == session_id) {
            if (focused_window_ == it->first) focused_window_ = 0;
            if (clipboard_owner_ == it->first) clipboard_owner_ = 0;
            it = windows_.erase(it);
        } else {
            // A media producer that disconnects stops feeding its window.
            if (it->second.attached_session == session_id) {
                it->second.attached_session = 0;
            }
            ++it;
        }
    }

    // A session may own a parent while another session owns a child. Remove
    // orphaned descendants after the owner set has been pruned.
    bool removed = true;
    while (removed) {
        removed = false;
        for (auto it = windows_.begin(); it != windows_.end();) {
            if (it->second.parent != 0 &&
                windows_.find(it->second.parent) == windows_.end()) {
                if (focused_window_ == it->first) focused_window_ = 0;
                if (clipboard_owner_ == it->first) clipboard_owner_ = 0;
                it = windows_.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
    }
    if (focused_window_ != 0 && windows_.find(focused_window_) == windows_.end()) {
        focused_window_ = 0;
    }
    if (clipboard_owner_ != 0 && windows_.find(clipboard_owner_) == windows_.end()) {
        clipboard_owner_ = 0;
    }
}

bool ControlState::apply(uint64_t session_id,
                         const KopmsControlCommandPayload& command,
                         const std::vector<uint8_t>& data,
                         KopmsControlAckPayload* ack, std::string* error) {
    if (!ack) {
        if (error) *error = "control ACK target is null";
        return false;
    }
    if (data.size() != command.data_size || data.size() > KOPMS_CONTROL_DATA_MAX) {
        if (error) *error = "control data does not match its command";
        return false;
    }

    *ack = {};
    ack->struct_size = KOPMS_CONTROL_ACK_PAYLOAD_SIZE;
    ack->operation = command.operation;
    ack->object_id = command.object_id;
    ack->related_id = command.related_id;
    set_status(ack, KOPMS_CONTROL_STATUS_INVALID);

    if (!session_active(session_id)) {
        set_status(ack, KOPMS_CONTROL_STATUS_PERMISSION);
        return true;
    }
    if (command.object_id == 0 && command.operation != KOPMS_CONTROL_FOCUS_SET &&
        command.operation != KOPMS_CONTROL_CLIPBOARD_SET_OWNER) {
        set_status(ack, KOPMS_CONTROL_STATUS_INVALID);
        return true;
    }

    switch (command.operation) {
        case KOPMS_CONTROL_WINDOW_CREATE: {
            if (windows_.find(command.object_id) != windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_CONFLICT);
                return true;
            }
            if (command.related_id != 0 &&
                windows_.find(command.related_id) == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            Window window;
            window.owner_session = session_id;
            window.parent = command.related_id;
            window.generation = next_generation();
            windows_.emplace(command.object_id, window);
            set_status(ack, KOPMS_CONTROL_STATUS_OK, window.generation);
            return true;
        }
        case KOPMS_CONTROL_WINDOW_DESTROY: {
            const auto it = windows_.find(command.object_id);
            if (it == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            if (!owns_window(session_id, command.object_id)) {
                set_status(ack, KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            const bool has_child = std::any_of(
                windows_.begin(), windows_.end(), [&](const auto& item) {
                    return item.second.parent == command.object_id;
                });
            if (has_child) {
                set_status(ack, KOPMS_CONTROL_STATUS_CONFLICT);
                return true;
            }
            const uint64_t generation = next_generation();
            windows_.erase(it);
            if (focused_window_ == command.object_id) focused_window_ = 0;
            if (clipboard_owner_ == command.object_id) clipboard_owner_ = 0;
            set_status(ack, KOPMS_CONTROL_STATUS_OK, generation);
            return true;
        }
        case KOPMS_CONTROL_WINDOW_SET_PARENT: {
            const auto it = windows_.find(command.object_id);
            if (it == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            if (!owns_window(session_id, command.object_id)) {
                set_status(ack, KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            if (command.related_id != 0 &&
                windows_.find(command.related_id) == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            if (parent_would_cycle(command.object_id, command.related_id)) {
                set_status(ack, KOPMS_CONTROL_STATUS_CONFLICT);
                return true;
            }
            it->second.parent = command.related_id;
            it->second.generation = next_generation();
            set_status(ack, KOPMS_CONTROL_STATUS_OK, it->second.generation);
            return true;
        }
        case KOPMS_CONTROL_FOCUS_SET: {
            if (command.object_id != 0 &&
                !owns_window(session_id, command.object_id)) {
                set_status(ack, windows_.find(command.object_id) == windows_.end()
                                   ? KOPMS_CONTROL_STATUS_NOT_FOUND
                                   : KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            focused_window_ = command.object_id;
            set_status(ack, KOPMS_CONTROL_STATUS_OK, next_generation());
            return true;
        }
        case KOPMS_CONTROL_CLIPBOARD_SET_OWNER: {
            if (command.object_id != 0 &&
                !owns_window(session_id, command.object_id)) {
                set_status(ack, windows_.find(command.object_id) == windows_.end()
                                   ? KOPMS_CONTROL_STATUS_NOT_FOUND
                                   : KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            clipboard_owner_ = command.object_id;
            set_status(ack, KOPMS_CONTROL_STATUS_OK, next_generation());
            return true;
        }
        case KOPMS_CONTROL_OWNERSHIP_SET: {
            const auto it = windows_.find(command.object_id);
            if (it == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            if (!owns_window(session_id, command.object_id)) {
                set_status(ack, KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            if (command.value > KOPMS_OWNERSHIP_SHARED) {
                set_status(ack, KOPMS_CONTROL_STATUS_INVALID);
                return true;
            }
            it->second.ownership = static_cast<uint32_t>(command.value);
            it->second.generation = next_generation();
            set_status(ack, KOPMS_CONTROL_STATUS_OK, it->second.generation);
            return true;
        }
        case KOPMS_CONTROL_WINDOW_ATTACH: {
            const auto it = windows_.find(command.object_id);
            if (it == windows_.end()) {
                set_status(ack, KOPMS_CONTROL_STATUS_NOT_FOUND);
                return true;
            }
            if (!owns_window(session_id, command.object_id)) {
                set_status(ack, KOPMS_CONTROL_STATUS_PERMISSION);
                return true;
            }
            if (command.value > 1) {
                set_status(ack, KOPMS_CONTROL_STATUS_INVALID);
                return true;
            }
            // value=1 binds this session as the window's native media
            // producer; value=0 detaches. The binding is what makes
            // FRAME_SUBMIT frames from this session land in this window.
            it->second.attached_session = command.value ? session_id : 0;
            it->second.generation = next_generation();
            set_status(ack, KOPMS_CONTROL_STATUS_OK, it->second.generation);
            return true;
        }
        default:
            set_status(ack, KOPMS_CONTROL_STATUS_INVALID);
            return true;
    }
}

std::vector<ControlState::WindowInfo> ControlState::snapshot() const {
    std::vector<WindowInfo> result;
    result.reserve(windows_.size());
    for (const auto& item : windows_) {
        WindowInfo info;
        info.id = item.first;
        info.parent = item.second.parent;
        info.owner_session = item.second.owner_session;
        info.attached_session = item.second.attached_session;
        info.ownership = item.second.ownership;
        info.generation = item.second.generation;
        result.push_back(info);
    }
    return result;
}

uint64_t ControlState::next_generation() {
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    return generation_;
}

bool ControlState::session_active(uint64_t session_id) const {
    return session_id != 0 && sessions_.find(session_id) != sessions_.end();
}

bool ControlState::owns_window(uint64_t session_id, uint64_t window_id) const {
    const auto it = windows_.find(window_id);
    return it != windows_.end() && it->second.owner_session == session_id;
}

bool ControlState::parent_would_cycle(uint64_t window_id, uint64_t parent_id) const {
    for (uint64_t current = parent_id; current != 0;) {
        if (current == window_id) return true;
        const auto it = windows_.find(current);
        if (it == windows_.end()) return false;
        current = it->second.parent;
    }
    return false;
}

void ControlState::set_status(KopmsControlAckPayload* ack,
                              KopmsControlStatus status, uint64_t generation) {
    ack->status = static_cast<uint32_t>(status);
    ack->generation = generation;
}

}  // namespace kopms
