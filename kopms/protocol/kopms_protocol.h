// KOPMS-S/KOPMS-C wire protocol primitives.
//
// The transport carries a fixed little-endian header followed by an extensible
// payload. File descriptors are sent separately with SCM_RIGHTS and are
// referred to by indexes in the frame payload; raw pointers never cross this
// boundary.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kopaw_abi.h"

struct KopmsFrameDescriptor;

#define KOPMS_PROTOCOL_MAGIC UINT32_C(0x4b4f504d)
#define KOPMS_PROTOCOL_MAJOR UINT16_C(1)
/// 1.1: Handle/modifier/explicit-sync capability negotiation (P3-M3/M4). The
/// 32-byte header, the CONTROL/MEDIA lanes, and SCM_RIGHTS framing are
/// unchanged; only new capability bits and their validation rules were added.
/// 1.2: optional colorimetry/HDR tail on FRAME_SUBMIT.
#define KOPMS_PROTOCOL_MINOR UINT16_C(2)

#define KOPMS_PROTOCOL_HEADER_SIZE UINT32_C(32)
#define KOPMS_PROTOCOL_MAX_PAYLOAD UINT32_C(65536)
#define KOPMS_PROTOCOL_MAX_FDS UINT32_C(KOPAW_MAX_DMABUF_PLANES + 1)

#define KOPMS_PROTOCOL_CAP_DMABUF UINT64_C(1)
#define KOPMS_PROTOCOL_CAP_FRAME_RELEASE UINT64_C(2)
#define KOPMS_PROTOCOL_CAP_WAYLAND_BRIDGE UINT64_C(4)
#define KOPMS_PROTOCOL_CAP_CONTROL_STATE UINT64_C(8)
/// KOPAW_MEMORY_VULKAN external-memory frames are addressable end to end.
#define KOPMS_PROTOCOL_CAP_HANDLE_FRAMES UINT64_C(16)
/// DRM format modifiers are negotiated instead of implicit tiling.
#define KOPMS_PROTOCOL_CAP_MODIFIERS UINT64_C(32)
/// Acquire fences may ride on FRAME_SUBMIT and release timing is honored
/// (release only after present/flip completion).
#define KOPMS_PROTOCOL_CAP_EXPLICIT_SYNC UINT64_C(64)
/// KopawColorMetadata is present in the optional FRAME_SUBMIT tail.
#define KOPMS_PROTOCOL_CAP_COLOR_METADATA UINT64_C(128)

#define KOPMS_HELLO_PAYLOAD_SIZE UINT32_C(32)
#define KOPMS_HELLO_ACK_PAYLOAD_SIZE UINT32_C(32)
#define KOPMS_FRAME_SUBMIT_BASE_SIZE UINT32_C(160)
#define KOPMS_FRAME_SUBMIT_COLOR_METADATA_SIZE UINT32_C(80)
#define KOPMS_FRAME_SUBMIT_PAYLOAD_SIZE \
    (KOPMS_FRAME_SUBMIT_BASE_SIZE + KOPMS_FRAME_SUBMIT_COLOR_METADATA_SIZE)
#define KOPMS_FRAME_RELEASE_PAYLOAD_SIZE UINT32_C(16)
#define KOPMS_GOODBYE_PAYLOAD_SIZE UINT32_C(8)
#define KOPMS_CONTROL_PAYLOAD_SIZE UINT32_C(40)
#define KOPMS_CONTROL_ACK_PAYLOAD_SIZE UINT32_C(48)
#define KOPMS_CONTROL_DATA_MAX UINT32_C(256)
#define KOPMS_ERROR_PREFIX_SIZE UINT32_C(16)
#define KOPMS_ERROR_TEXT_MAX UINT32_C(96)

typedef enum KopmsMessageType {
    KOPMS_MESSAGE_HELLO = 1,
    KOPMS_MESSAGE_HELLO_ACK = 2,
    KOPMS_MESSAGE_ERROR = 3,
    KOPMS_MESSAGE_FRAME_SUBMIT = 4,
    KOPMS_MESSAGE_FRAME_RELEASE = 5,
    KOPMS_MESSAGE_PING = 6,
    KOPMS_MESSAGE_PONG = 7,
    KOPMS_MESSAGE_GOODBYE = 8,
    KOPMS_MESSAGE_CONTROL_COMMAND = 9,
    KOPMS_MESSAGE_CONTROL_ACK = 10,
} KopmsMessageType;

#define KOPMS_MESSAGE_FLAG_NONE UINT16_C(0)
#define KOPMS_MESSAGE_FLAG_REPLY UINT16_C(1)
#define KOPMS_MESSAGE_FLAG_CONTROL UINT16_C(2)
#define KOPMS_MESSAGE_FLAG_MEDIA UINT16_C(4)

typedef enum KopmsControlOperation {
    KOPMS_CONTROL_WINDOW_CREATE = 1,
    KOPMS_CONTROL_WINDOW_DESTROY = 2,
    KOPMS_CONTROL_WINDOW_SET_PARENT = 3,
    KOPMS_CONTROL_FOCUS_SET = 4,
    KOPMS_CONTROL_CLIPBOARD_SET_OWNER = 5,
    KOPMS_CONTROL_OWNERSHIP_SET = 6,
    // Bind the sending session as the window's native media producer (M4):
    // value=1 attaches, value=0 detaches. Only the owner session may attach.
    KOPMS_CONTROL_WINDOW_ATTACH = 7,
} KopmsControlOperation;

typedef enum KopmsControlStatus {
    KOPMS_CONTROL_STATUS_OK = 0,
    KOPMS_CONTROL_STATUS_INVALID = 1,
    KOPMS_CONTROL_STATUS_NOT_FOUND = 2,
    KOPMS_CONTROL_STATUS_PERMISSION = 3,
    KOPMS_CONTROL_STATUS_CONFLICT = 4,
} KopmsControlStatus;

typedef enum KopmsOwnershipState {
    KOPMS_OWNERSHIP_RELEASED = 0,
    KOPMS_OWNERSHIP_EXCLUSIVE = 1,
    KOPMS_OWNERSHIP_SHARED = 2,
} KopmsOwnershipState;

typedef enum KopmsErrorCode {
    KOPMS_ERROR_MALFORMED = 1,
    KOPMS_ERROR_VERSION = 2,
    KOPMS_ERROR_UNSUPPORTED = 3,
    KOPMS_ERROR_LIMIT = 4,
    KOPMS_ERROR_FRAME = 5,
    KOPMS_ERROR_SEQUENCE = 6,
} KopmsErrorCode;

typedef enum KopmsFrameReleaseStatus {
    KOPMS_FRAME_RELEASE_OK = 0,
    KOPMS_FRAME_RELEASE_REJECTED = 1,
    KOPMS_FRAME_RELEASE_DROPPED = 2,
} KopmsFrameReleaseStatus;

// This is a host-side view. It is encoded by encode_message_header(), so its
// C++ padding is never part of the wire format.
typedef struct KopmsMessageHeader {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint16_t type;
    uint16_t flags;
    uint64_t sequence;
    uint32_t payload_size;
    uint32_t fd_count;
} KopmsMessageHeader;

typedef struct KopmsHelloPayload {
    uint32_t struct_size;
    uint32_t protocol_major;
    uint32_t protocol_minor;
    uint64_t capabilities;
    uint32_t max_payload;
    uint32_t max_fds;
} KopmsHelloPayload;

typedef struct KopmsHelloAckPayload {
    uint32_t struct_size;
    uint32_t status;
    uint32_t selected_major;
    uint32_t selected_minor;
    uint64_t capabilities;
    uint32_t max_payload;
    uint32_t max_fds;
} KopmsHelloAckPayload;

typedef struct KopmsFramePlane {
    int32_t fd_index;
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
} KopmsFramePlane;

typedef struct KopmsFrameFence {
    uint32_t kind;
    int32_t fd_index;
    uint64_t value;
} KopmsFrameFence;

typedef struct KopmsFrameSubmitPayload {
    uint32_t struct_size;
    uint32_t frame_id;
    uint32_t media_type;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t memory_type;
    uint32_t plane_count;
    int64_t pts;
    int64_t dts;
    KopmsFramePlane planes[KOPAW_MAX_DMABUF_PLANES];
    KopmsFrameFence acquire_fence;
    // Optional 1.2 tail. Presence is gated by struct_size and the negotiated
    // KOPMS_PROTOCOL_CAP_COLOR_METADATA capability.
    KopawColorMetadata color;
} KopmsFrameSubmitPayload;

typedef struct KopmsFrameReleasePayload {
    uint32_t struct_size;
    uint32_t frame_id;
    uint32_t status;
    uint32_t reserved;
} KopmsFrameReleasePayload;

typedef struct KopmsGoodbyePayload {
    uint32_t struct_size;
    uint32_t reason;
} KopmsGoodbyePayload;

// Generic control command. object_id identifies a window/object; related_id
// is its parent or another operation-specific object; value carries a state
// or opaque offer token; data is an optional bounded D-Bus/control-plane blob.
typedef struct KopmsControlCommandPayload {
    uint32_t struct_size;
    uint32_t operation;
    uint64_t object_id;
    uint64_t related_id;
    uint64_t value;
    uint32_t flags;
    uint32_t data_size;
} KopmsControlCommandPayload;

typedef struct KopmsControlAckPayload {
    uint32_t struct_size;
    uint32_t operation;
    uint32_t status;
    uint32_t reserved;
    uint64_t request_sequence;
    uint64_t object_id;
    uint64_t related_id;
    uint64_t generation;
} KopmsControlAckPayload;

#ifdef __cplusplus
static_assert(sizeof(KopmsMessageHeader) == 32, "unexpected KOPMS header layout");
static_assert(sizeof(KopmsFramePlane) == 24, "unexpected KOPMS plane layout");
static_assert(sizeof(KopmsFrameFence) == 16, "unexpected KOPMS fence layout");
static_assert(sizeof(KopawHdrMetadata) == 56, "unexpected KOPAW HDR layout");
static_assert(sizeof(KopawColorMetadata) == 80, "unexpected KOPAW color layout");
static_assert(sizeof(KopmsFrameSubmitPayload) == KOPMS_FRAME_SUBMIT_PAYLOAD_SIZE,
              "unexpected KOPMS frame layout");
static_assert(sizeof(KopmsControlCommandPayload) == KOPMS_CONTROL_PAYLOAD_SIZE,
              "unexpected KOPMS control layout");
static_assert(sizeof(KopmsControlAckPayload) == KOPMS_CONTROL_ACK_PAYLOAD_SIZE,
              "unexpected KOPMS control ack layout");

namespace kopms {

using WireHeader = std::array<uint8_t, KOPMS_PROTOCOL_HEADER_SIZE>;

void encode_message_header(const KopmsMessageHeader& header, WireHeader* output);
bool decode_message_header(const uint8_t* data, size_t size,
                           KopmsMessageHeader* header, std::string* error);

std::vector<uint8_t> encode_hello(const KopmsHelloPayload& hello);
bool decode_hello(const std::vector<uint8_t>& payload, KopmsHelloPayload* hello,
                  std::string* error);

std::vector<uint8_t> encode_hello_ack(const KopmsHelloAckPayload& ack);
bool decode_hello_ack(const std::vector<uint8_t>& payload,
                      KopmsHelloAckPayload* ack, std::string* error);

std::vector<uint8_t> encode_frame_submit(const KopmsFrameSubmitPayload& frame);
bool decode_frame_submit(const std::vector<uint8_t>& payload,
                         KopmsFrameSubmitPayload* frame, std::string* error);

std::vector<uint8_t> encode_frame_release(const KopmsFrameReleasePayload& release);
bool decode_frame_release(const std::vector<uint8_t>& payload,
                          KopmsFrameReleasePayload* release, std::string* error);

std::vector<uint8_t> encode_goodbye(const KopmsGoodbyePayload& goodbye);
bool decode_goodbye(const std::vector<uint8_t>& payload,
                    KopmsGoodbyePayload* goodbye, std::string* error);

std::vector<uint8_t> encode_control_command(
    const KopmsControlCommandPayload& command,
    const std::vector<uint8_t>& data);
bool decode_control_command(const std::vector<uint8_t>& payload,
                            KopmsControlCommandPayload* command,
                            std::vector<uint8_t>* data, std::string* error);

std::vector<uint8_t> encode_control_ack(const KopmsControlAckPayload& ack);
bool decode_control_ack(const std::vector<uint8_t>& payload,
                        KopmsControlAckPayload* ack, std::string* error);

std::vector<uint8_t> encode_error(KopmsErrorCode code, uint16_t offending_type,
                                  const std::string& message);
bool decode_error(const std::vector<uint8_t>& payload, KopmsErrorCode* code,
                  uint16_t* offending_type, std::string* message,
                  std::string* error);

bool validate_frame_submit(const KopmsFrameSubmitPayload& frame, size_t fd_count,
                           std::string* error);
// Capability-aware validation used by KOPMS-S after HELLO. negotiated_caps is
// the HELLO_ACK capability intersection for the session.
bool validate_frame_submit_caps(const KopmsFrameSubmitPayload& frame, size_t fd_count,
                                uint64_t negotiated_caps, std::string* error);
bool make_frame_submit(const KopmsFrameDescriptor* frame, uint32_t frame_id,
                       KopmsFrameSubmitPayload* output, std::vector<int>* fds,
                       std::string* error);

}  // namespace kopms
#endif
