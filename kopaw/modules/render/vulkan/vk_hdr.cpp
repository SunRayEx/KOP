#include "vk_hdr.hpp"

namespace kopaw {

bool surface_supports_hdr10(const VkSurfaceFormatKHR* fmts, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
            fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            return true;
        }
    }
    return false;
}

SurfaceOutput select_surface_output(const VkSurfaceFormatKHR* fmts, uint32_t count,
                                    HdrMode mode, bool content_hdr) {
    SurfaceOutput out;
    const bool want_hdr =
        mode == HdrMode::On || (mode == HdrMode::Auto && content_hdr);
    if (want_hdr && surface_supports_hdr10(fmts, count)) {
        out.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        out.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
        out.hdr10 = true;
        return out;
    }
    // SDR：优先 8-bit UNORM + sRGB，否则取驱动首选项。
    for (uint32_t i = 0; i < count; ++i) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
             fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            out.format = fmts[i].format;
            out.color_space = fmts[i].colorSpace;
            return out;
        }
    }
    if (count > 0) {
        out.format = fmts[0].format;
        out.color_space = fmts[0].colorSpace;
    }
    return out;
}

}  // namespace kopaw
