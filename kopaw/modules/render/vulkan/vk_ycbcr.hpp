#pragma once

#include <string>
#include <vulkan/vulkan.h>

#include "kopaw_abi.h"

namespace kopaw {

struct YcbcrConfig {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t modifier = 0;
    VkSamplerYcbcrModelConversion model =
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
    VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    VkChromaLocation x_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
    VkChromaLocation y_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
    VkFilter filter = VK_FILTER_NEAREST;
    uint32_t descriptor_count = 0;
    VkExtent2D max_extent{};
};

// Native YCbCr frames must carry the 5.3 color tail. Matrix/range are never
// derived from dimensions; unsupported or absent values make the caller use
// its CPU fallback path.
bool describe_ycbcr_frame(const KopawFrame* frame, YcbcrConfig* config,
                          std::string* error);

// Query the actual DMA-BUF modifier, not optimal-tiling format capabilities.
bool query_ycbcr_support(VkPhysicalDevice physical, uint32_t width,
                         uint32_t height, YcbcrConfig* config,
                         std::string* error);

} // namespace kopaw
