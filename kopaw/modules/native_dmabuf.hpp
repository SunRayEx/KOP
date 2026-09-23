// Native decoder surface formats that a downstream DMA-BUF consumer can
// accept. The mask is an in-process startup contract; the exact DRM modifier
// is still checked against the concrete frame at import time.
#pragma once

#include <cstdint>

namespace kopaw {

constexpr uint32_t kNativeDmabufFormatNone = 0;
constexpr uint32_t kNativeDmabufFormatNv12 = 1u << 0;
constexpr uint32_t kNativeDmabufFormatP010 = 1u << 1;
constexpr uint32_t kNativeDmabufFormatKnown =
    kNativeDmabufFormatNv12 | kNativeDmabufFormatP010;

inline bool native_dmabuf_format_supported(uint32_t formats, uint32_t format) {
    return (formats & format) != 0;
}

}  // namespace kopaw
