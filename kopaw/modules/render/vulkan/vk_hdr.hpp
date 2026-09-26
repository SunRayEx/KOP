// 显示能力协商：在表面可用的 (VkFormat, VkColorSpaceKHR) 对里选出交换链输出
// 配置。HDR10 首选 A2B10G10R10 + ST.2084（HDR10 标准组合）；不可用时安全
// 回退到 8-bit UNORM + sRGB。逻辑纯函数、无设备依赖，单元测试用合成格式表
// 覆盖各分支（vk_hdr_test）。
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "../hdr_mode.hpp"

namespace kopaw {

// 交换链输出配置。
struct SurfaceOutput {
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool hdr10 = false;  // true = HDR10（PQ 编码 Rec.2020）
};

// 表面是否具备 HDR10 能力（存在 A2B10G10R10 + HDR10_ST2084 对）。
bool surface_supports_hdr10(const VkSurfaceFormatKHR* fmts, uint32_t count);

// 按用户模式、内容是否 HDR、表面可用格式选出输出配置：
//   Off               → 永远 SDR
//   On + 支持         → HDR10，否则 SDR
//   Auto + 内容 HDR + 支持 → HDR10，否则 SDR
// count == 0 时安全回退 SDR（fmts 可为 nullptr）。
SurfaceOutput select_surface_output(const VkSurfaceFormatKHR* fmts, uint32_t count,
                                    HdrMode mode, bool content_hdr);

}  // namespace kopaw
