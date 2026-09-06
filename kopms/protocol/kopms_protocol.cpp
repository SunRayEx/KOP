#include "kopms_protocol.h"

#include "frame_bridge.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace kopms {

namespace {

constexpr size_t kHeaderSize = KOPMS_PROTOCOL_HEADER_SIZE;
constexpr size_t kHelloSize = KOPMS_HELLO_PAYLOAD_SIZE;
constexpr size_t kHelloAckSize = KOPMS_HELLO_ACK_PAYLOAD_SIZE;
constexpr size_t kFrameSize = KOPMS_FRAME_SUBMIT_PAYLOAD_SIZE;
constexpr size_t kReleaseSize = KOPMS_FRAME_RELEASE_PAYLOAD_SIZE;
constexpr size_t kGoodbyeSize = KOPMS_GOODBYE_PAYLOAD_SIZE;
constexpr size_t kErrorPrefixSize = KOPMS_ERROR_PREFIX_SIZE;

void put_u16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t* data, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) {
        data[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void put_u64(uint8_t* data, uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) {
        data[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint16_t get_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint32_t get_u32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(data[i]) << (i * 8);
    }
    return value;
}

uint64_t get_u64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

void set_error(std::string* error, const char* message) {
    if (error) *error = message;
}

bool check_struct(const std::vector<uint8_t>& payload, size_t minimum,
                 uint32_t* struct_size, std::string* error) {
    if (payload.size() < minimum) {
        set_error(error, "payload too small");
        return false;
    }
    const uint32_t size = get_u32(payload.data());
    if (size < minimum || size > payload.size()) {
        set_error(error, "payload struct_size is invalid");
        return false;
    }
    if (struct_size) *struct_size = size;
    return true;
}

}  // namespace

void encode_message_header(const KopmsMessageHeader& header, WireHeader* output) {
    if (!output) return;
    output->fill(0);
    put_u32(output->data(), header.magic);
    put_u16(output->data() + 4, header.major);
    put_u16(output->data() + 6, header.minor);
    put_u16(output->data() + 8, header.type);
    put_u16(output->data() + 10, header.flags);
    put_u64(output->data() + 16, header.sequence);
    put_u32(output->data() + 24, header.payload_size);
    put_u32(output->data() + 28, header.fd_count);
}

bool decode_message_header(const uint8_t* data, size_t size,
                           KopmsMessageHeader* header, std::string* error) {
    if (!data || !header || size < kHeaderSize) {
        set_error(error, "message header is truncated");
        return false;
    }
    header->magic = get_u32(data);
    header->major = get_u16(data + 4);
    header->minor = get_u16(data + 6);
    header->type = get_u16(data + 8);
    header->flags = get_u16(data + 10);
    header->sequence = get_u64(data + 16);
    header->payload_size = get_u32(data + 24);
    header->fd_count = get_u32(data + 28);
    if (header->magic != KOPMS_PROTOCOL_MAGIC) {
        set_error(error, "message magic is invalid");
        return false;
    }
    if (header->payload_size > KOPMS_PROTOCOL_MAX_PAYLOAD) {
        set_error(error, "message payload exceeds the limit");
        return false;
    }
    if (header->fd_count > KOPMS_PROTOCOL_MAX_FDS) {
        set_error(error, "message fd count exceeds the limit");
        return false;
    }
    return true;
}

std::vector<uint8_t> encode_hello(const KopmsHelloPayload& hello) {
    std::vector<uint8_t> payload(kHelloSize, 0);
    put_u32(payload.data(), hello.struct_size);
    put_u32(payload.data() + 4, hello.protocol_major);
    put_u32(payload.data() + 8, hello.protocol_minor);
    put_u64(payload.data() + 16, hello.capabilities);
    put_u32(payload.data() + 24, hello.max_payload);
    put_u32(payload.data() + 28, hello.max_fds);
    return payload;
}

bool decode_hello(const std::vector<uint8_t>& payload, KopmsHelloPayload* hello,
                  std::string* error) {
    if (!hello || !check_struct(payload, kHelloSize, nullptr, error)) return false;
    hello->struct_size = get_u32(payload.data());
    hello->protocol_major = get_u32(payload.data() + 4);
    hello->protocol_minor = get_u32(payload.data() + 8);
    hello->capabilities = get_u64(payload.data() + 16);
    hello->max_payload = get_u32(payload.data() + 24);
    hello->max_fds = get_u32(payload.data() + 28);
    return true;
}

std::vector<uint8_t> encode_hello_ack(const KopmsHelloAckPayload& ack) {
    std::vector<uint8_t> payload(kHelloAckSize, 0);
    put_u32(payload.data(), ack.struct_size);
    put_u32(payload.data() + 4, ack.status);
    put_u32(payload.data() + 8, ack.selected_major);
    put_u32(payload.data() + 12, ack.selected_minor);
    put_u64(payload.data() + 16, ack.capabilities);
    put_u32(payload.data() + 24, ack.max_payload);
    put_u32(payload.data() + 28, ack.max_fds);
    return payload;
}

bool decode_hello_ack(const std::vector<uint8_t>& payload,
                      KopmsHelloAckPayload* ack, std::string* error) {
    if (!ack || !check_struct(payload, kHelloAckSize, nullptr, error)) return false;
    ack->struct_size = get_u32(payload.data());
    ack->status = get_u32(payload.data() + 4);
    ack->selected_major = get_u32(payload.data() + 8);
    ack->selected_minor = get_u32(payload.data() + 12);
    ack->capabilities = get_u64(payload.data() + 16);
    ack->max_payload = get_u32(payload.data() + 24);
    ack->max_fds = get_u32(payload.data() + 28);
    return true;
}

std::vector<uint8_t> encode_frame_submit(const KopmsFrameSubmitPayload& frame) {
    std::vector<uint8_t> payload(kFrameSize, 0);
    put_u32(payload.data(), frame.struct_size);
    put_u32(payload.data() + 4, frame.frame_id);
    put_u32(payload.data() + 8, frame.media_type);
    put_u32(payload.data() + 12, frame.width);
    put_u32(payload.data() + 16, frame.height);
    put_u32(payload.data() + 20, frame.format);
    put_u32(payload.data() + 24, frame.memory_type);
    put_u32(payload.data() + 28, frame.plane_count);
    put_u64(payload.data() + 32, static_cast<uint64_t>(frame.pts));
    put_u64(payload.data() + 40, static_cast<uint64_t>(frame.dts));
    for (uint32_t i = 0; i < KOPAW_MAX_DMABUF_PLANES; ++i) {
        const size_t offset = 48 + static_cast<size_t>(i) * 24;
        put_u32(payload.data() + offset,
                static_cast<uint32_t>(frame.planes[i].fd_index));
        put_u32(payload.data() + offset + 4, frame.planes[i].offset);
        put_u32(payload.data() + offset + 8, frame.planes[i].stride);
        put_u64(payload.data() + offset + 16, frame.planes[i].modifier);
    }
    put_u32(payload.data() + 144, frame.acquire_fence.kind);
    put_u32(payload.data() + 148,
            static_cast<uint32_t>(frame.acquire_fence.fd_index));
    put_u64(payload.data() + 152, frame.acquire_fence.value);
    return payload;
}

bool decode_frame_submit(const std::vector<uint8_t>& payload,
                         KopmsFrameSubmitPayload* frame, std::string* error) {
    if (!frame || !check_struct(payload, kFrameSize, nullptr, error)) return false;
    std::memset(frame, 0, sizeof(*frame));
    frame->struct_size = get_u32(payload.data());
    frame->frame_id = get_u32(payload.data() + 4);
    frame->media_type = get_u32(payload.data() + 8);
    frame->width = get_u32(payload.data() + 12);
    frame->height = get_u32(payload.data() + 16);
    frame->format = get_u32(payload.data() + 20);
    frame->memory_type = get_u32(payload.data() + 24);
    frame->plane_count = get_u32(payload.data() + 28);
    frame->pts = static_cast<int64_t>(get_u64(payload.data() + 32));
    frame->dts = static_cast<int64_t>(get_u64(payload.data() + 40));
    for (uint32_t i = 0; i < KOPAW_MAX_DMABUF_PLANES; ++i) {
        const size_t offset = 48 + static_cast<size_t>(i) * 24;
        frame->planes[i].fd_index = static_cast<int32_t>(get_u32(payload.data() + offset));
        frame->planes[i].offset = get_u32(payload.data() + offset + 4);
        frame->planes[i].stride = get_u32(payload.data() + offset + 8);
        frame->planes[i].modifier = get_u64(payload.data() + offset + 16);
    }
    frame->acquire_fence.kind = get_u32(payload.data() + 144);
    frame->acquire_fence.fd_index = static_cast<int32_t>(get_u32(payload.data() + 148));
    frame->acquire_fence.value = get_u64(payload.data() + 152);
    return true;
}

std::vector<uint8_t> encode_frame_release(const KopmsFrameReleasePayload& release) {
    std::vector<uint8_t> payload(kReleaseSize, 0);
    put_u32(payload.data(), release.struct_size);
    put_u32(payload.data() + 4, release.frame_id);
    put_u32(payload.data() + 8, release.status);
    put_u32(payload.data() + 12, release.reserved);
    return payload;
}

bool decode_frame_release(const std::vector<uint8_t>& payload,
                          KopmsFrameReleasePayload* release, std::string* error) {
    if (!release || !check_struct(payload, kReleaseSize, nullptr, error)) return false;
    release->struct_size = get_u32(payload.data());
    release->frame_id = get_u32(payload.data() + 4);
    release->status = get_u32(payload.data() + 8);
    release->reserved = get_u32(payload.data() + 12);
    return true;
}

std::vector<uint8_t> encode_goodbye(const KopmsGoodbyePayload& goodbye) {
    std::vector<uint8_t> payload(kGoodbyeSize, 0);
    put_u32(payload.data(), goodbye.struct_size);
    put_u32(payload.data() + 4, goodbye.reason);
    return payload;
}

bool decode_goodbye(const std::vector<uint8_t>& payload,
                    KopmsGoodbyePayload* goodbye, std::string* error) {
    if (!goodbye || !check_struct(payload, kGoodbyeSize, nullptr, error)) return false;
    goodbye->struct_size = get_u32(payload.data());
    goodbye->reason = get_u32(payload.data() + 4);
    return true;
}

std::vector<uint8_t> encode_control_command(
    const KopmsControlCommandPayload& command,
    const std::vector<uint8_t>& data) {
    const size_t data_size = std::min<size_t>(data.size(), KOPMS_CONTROL_DATA_MAX);
    std::vector<uint8_t> payload(KOPMS_CONTROL_PAYLOAD_SIZE + data_size, 0);
    put_u32(payload.data(), static_cast<uint32_t>(payload.size()));
    put_u32(payload.data() + 4, command.operation);
    put_u64(payload.data() + 8, command.object_id);
    put_u64(payload.data() + 16, command.related_id);
    put_u64(payload.data() + 24, command.value);
    put_u32(payload.data() + 32, command.flags);
    put_u32(payload.data() + 36, static_cast<uint32_t>(data_size));
    if (data_size != 0) {
        std::memcpy(payload.data() + KOPMS_CONTROL_PAYLOAD_SIZE, data.data(), data_size);
    }
    return payload;
}

bool decode_control_command(const std::vector<uint8_t>& payload,
                            KopmsControlCommandPayload* command,
                            std::vector<uint8_t>* data, std::string* error) {
    uint32_t struct_size = 0;
    if (!command || !data ||
        !check_struct(payload, KOPMS_CONTROL_PAYLOAD_SIZE, &struct_size, error)) {
        return false;
    }
    const uint32_t data_size = get_u32(payload.data() + 36);
    if (data_size > KOPMS_CONTROL_DATA_MAX ||
        KOPMS_CONTROL_PAYLOAD_SIZE + data_size > struct_size ||
        KOPMS_CONTROL_PAYLOAD_SIZE + data_size > payload.size()) {
        set_error(error, "control data length is invalid");
        return false;
    }
    command->struct_size = struct_size;
    command->operation = get_u32(payload.data() + 4);
    command->object_id = get_u64(payload.data() + 8);
    command->related_id = get_u64(payload.data() + 16);
    command->value = get_u64(payload.data() + 24);
    command->flags = get_u32(payload.data() + 32);
    command->data_size = data_size;
    data->assign(payload.begin() + KOPMS_CONTROL_PAYLOAD_SIZE,
                 payload.begin() + KOPMS_CONTROL_PAYLOAD_SIZE + data_size);
    return true;
}

std::vector<uint8_t> encode_control_ack(const KopmsControlAckPayload& ack) {
    std::vector<uint8_t> payload(KOPMS_CONTROL_ACK_PAYLOAD_SIZE, 0);
    put_u32(payload.data(), ack.struct_size);
    put_u32(payload.data() + 4, ack.operation);
    put_u32(payload.data() + 8, ack.status);
    put_u32(payload.data() + 12, ack.reserved);
    put_u64(payload.data() + 16, ack.request_sequence);
    put_u64(payload.data() + 24, ack.object_id);
    put_u64(payload.data() + 32, ack.related_id);
    put_u64(payload.data() + 40, ack.generation);
    return payload;
}

bool decode_control_ack(const std::vector<uint8_t>& payload,
                        KopmsControlAckPayload* ack, std::string* error) {
    if (!ack || !check_struct(payload, KOPMS_CONTROL_ACK_PAYLOAD_SIZE, nullptr, error)) {
        return false;
    }
    ack->struct_size = get_u32(payload.data());
    ack->operation = get_u32(payload.data() + 4);
    ack->status = get_u32(payload.data() + 8);
    ack->reserved = get_u32(payload.data() + 12);
    ack->request_sequence = get_u64(payload.data() + 16);
    ack->object_id = get_u64(payload.data() + 24);
    ack->related_id = get_u64(payload.data() + 32);
    ack->generation = get_u64(payload.data() + 40);
    return true;
}

std::vector<uint8_t> encode_error(KopmsErrorCode code, uint16_t offending_type,
                                  const std::string& message) {
    const size_t text_size = std::min<size_t>(message.size(), KOPMS_ERROR_TEXT_MAX);
    std::vector<uint8_t> payload(kErrorPrefixSize + text_size, 0);
    put_u32(payload.data(), static_cast<uint32_t>(payload.size()));
    put_u32(payload.data() + 4, static_cast<uint32_t>(code));
    put_u32(payload.data() + 8, offending_type);
    put_u32(payload.data() + 12, static_cast<uint32_t>(text_size));
    std::memcpy(payload.data() + kErrorPrefixSize, message.data(), text_size);
    return payload;
}

bool decode_error(const std::vector<uint8_t>& payload, KopmsErrorCode* code,
                  uint16_t* offending_type, std::string* message,
                  std::string* error) {
    uint32_t struct_size = 0;
    if (!code || !offending_type || !message ||
        !check_struct(payload, kErrorPrefixSize, &struct_size, error)) {
        return false;
    }
    const uint32_t text_size = get_u32(payload.data() + 12);
    if (text_size > KOPMS_ERROR_TEXT_MAX ||
        kErrorPrefixSize + text_size > struct_size ||
        kErrorPrefixSize + text_size > payload.size()) {
        set_error(error, "error text length is invalid");
        return false;
    }
    *code = static_cast<KopmsErrorCode>(get_u32(payload.data() + 4));
    *offending_type = static_cast<uint16_t>(get_u32(payload.data() + 8));
    message->assign(reinterpret_cast<const char*>(payload.data() + kErrorPrefixSize),
                    text_size);
    return true;
}

bool validate_frame_submit_caps(const KopmsFrameSubmitPayload& frame, size_t fd_count,
                                uint64_t negotiated_caps, std::string* error) {
    if (frame.struct_size < kFrameSize) {
        set_error(error, "frame struct_size is too small");
        return false;
    }
    if (frame.frame_id == 0) {
        set_error(error, "frame id must not be zero");
        return false;
    }
    if (frame.media_type != KOPAW_MEDIA_VIDEO && frame.media_type != KOPAW_MEDIA_AUDIO) {
        set_error(error, "frame media type is invalid");
        return false;
    }
    // The KOPMS wire path is deliberately GPU-facing. CPU wl_shm remains a
    // separate Wayland compatibility path until a future explicit bridge is
    // specified. KOPAW_MEMORY_VULKAN is DMA-BUF-backed external memory and is
    // accepted only when both ends negotiated the Handle capability.
    if (frame.memory_type == KOPAW_MEMORY_VULKAN) {
        if ((negotiated_caps & KOPMS_PROTOCOL_CAP_HANDLE_FRAMES) == 0) {
            set_error(error, "Vulkan external-memory frames require the Handle capability");
            return false;
        }
    } else if (frame.memory_type != KOPAW_MEMORY_DMABUF) {
        set_error(error, "KOPMS frames must use DMA-BUF or Vulkan external memory");
        return false;
    }
    if (frame.plane_count == 0 || frame.plane_count > KOPAW_MAX_DMABUF_PLANES) {
        set_error(error, "frame plane count is invalid");
        return false;
    }
    if (fd_count > KOPMS_PROTOCOL_MAX_FDS) {
        set_error(error, "frame fd count exceeds the limit");
        return false;
    }

    for (uint32_t i = 0; i < frame.plane_count; ++i) {
        if (frame.planes[i].modifier != 0 &&
            (negotiated_caps & KOPMS_PROTOCOL_CAP_MODIFIERS) == 0) {
            set_error(error, "plane modifiers require the modifier capability");
            return false;
        }
    }

    std::array<bool, KOPMS_PROTOCOL_MAX_FDS> used{};
    for (uint32_t i = 0; i < frame.plane_count; ++i) {
        const int32_t index = frame.planes[i].fd_index;
        if (index < 0 || static_cast<size_t>(index) >= fd_count || used[index]) {
            set_error(error, "frame plane fd indexes are invalid");
            return false;
        }
        used[index] = true;
    }

    size_t expected_fds = frame.plane_count;
    switch (frame.acquire_fence.kind) {
        case KOPAW_SYNC_FENCE_NONE:
        case KOPAW_SYNC_FENCE_TIMELINE:
            if (frame.acquire_fence.fd_index != -1) {
                set_error(error, "fence fd index must be -1");
                return false;
            }
            break;
        case KOPAW_SYNC_FENCE_FD: {
            if ((negotiated_caps & KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC) == 0) {
                set_error(error, "acquire fences require the explicit-sync capability");
                return false;
            }
            const int32_t index = frame.acquire_fence.fd_index;
            if (index < 0 || static_cast<size_t>(index) >= fd_count || used[index]) {
                set_error(error, "acquire fence fd index is invalid");
                return false;
            }
            used[index] = true;
            ++expected_fds;
            break;
        }
        default:
            set_error(error, "acquire fence kind is invalid");
            return false;
    }
    if (fd_count != expected_fds) {
        set_error(error, "frame fd count does not match its metadata");
        return false;
    }
    for (size_t i = 0; i < fd_count; ++i) {
        if (!used[i]) {
            set_error(error, "frame contains an unreferenced fd");
            return false;
        }
    }
    return true;
}

bool validate_frame_submit(const KopmsFrameSubmitPayload& frame, size_t fd_count,
                           std::string* error) {
    // Legacy callers do not negotiate; grant the full P3 capability set.
    return validate_frame_submit_caps(
        frame, fd_count,
        KOPMS_PROTOCOL_CAP_HANDLE_FRAMES | KOPMS_PROTOCOL_CAP_MODIFIERS |
            KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC,
        error);
}

bool make_frame_submit(const KopmsFrameDescriptor* frame, uint32_t frame_id,
                       KopmsFrameSubmitPayload* output, std::vector<int>* fds,
                       std::string* error) {
    if (!frame || !output || !fds) {
        set_error(error, "frame submit arguments are invalid");
        return false;
    }
    if (!validate_frame_descriptor(frame, error)) return false;
    if (frame->memory_type != KOPAW_MEMORY_DMABUF &&
        frame->memory_type != KOPAW_MEMORY_VULKAN) {
        set_error(error, "KOPMS-C only submits DMA-BUF/external-memory frames");
        return false;
    }

    // dma_buf_handle is intentionally not serialized. The wire keeps using
    // FD indexes plus SCM_RIGHTS so the server can materialize its own local
    // handle instead of observing a client process's numeric FD.

    std::memset(output, 0, sizeof(*output));
    output->struct_size = KOPMS_FRAME_SUBMIT_PAYLOAD_SIZE;
    output->frame_id = frame_id;
    output->media_type = static_cast<uint32_t>(frame->media_type);
    output->width = frame->width;
    output->height = frame->height;
    output->format = frame->format;
    output->memory_type = frame->memory_type;
    output->plane_count = frame->plane_count;
    const size_t timestamps_end =
        offsetof(KopmsFrameDescriptor, dts) + sizeof(frame->dts);
    if (frame->struct_size >= timestamps_end) {
        output->pts = frame->pts;
        output->dts = frame->dts;
    }
    fds->clear();
    fds->reserve(frame->plane_count + 1);
    for (uint32_t i = 0; i < frame->plane_count; ++i) {
        output->planes[i].offset = frame->planes[i].offset;
        output->planes[i].stride = frame->planes[i].stride;
        output->planes[i].modifier = frame->planes[i].modifier;
        // 多平面帧共享同一 DRM 对象（NV12 等）：相同 fd 只经 SCM_RIGHTS 发送
        // 一次并复用 fd_index——否则接收端会为同一 buffer 物化出多个 fd，
        // 无法识别"平面位于同一 DMA-BUF"的导入约束。
        int32_t index = -1;
        for (uint32_t j = 0; j < i; ++j) {
            if (frame->planes[j].fd == frame->planes[i].fd) {
                index = output->planes[j].fd_index;
                break;
            }
        }
        if (index < 0) {
            index = static_cast<int32_t>(fds->size());
            fds->push_back(frame->planes[i].fd);
        }
        output->planes[i].fd_index = index;
    }
    output->acquire_fence.kind = frame->acquire_fence.kind;
    output->acquire_fence.fd_index = -1;
    output->acquire_fence.value = frame->acquire_fence.value;
    if (frame->acquire_fence.kind == KOPAW_SYNC_FENCE_FD) {
        output->acquire_fence.fd_index = static_cast<int32_t>(fds->size());
        fds->push_back(frame->acquire_fence.fd);
    }
    return validate_frame_submit(*output, fds->size(), error);
}

}  // namespace kopms
