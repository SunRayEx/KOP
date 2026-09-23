#include "vk_ycbcr.hpp"

#include <cstddef>
#include <sys/stat.h>

#include <vector>

#include "../../frame.hpp"

namespace kopaw {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool same_object(int first, int second) {
    struct stat a{}, b{};
    return first >= 0 && second >= 0 && fstat(first, &a) == 0 &&
           fstat(second, &b) == 0 && a.st_dev == b.st_dev &&
           a.st_ino == b.st_ino;
}

bool map_color(const KopawFrame* frame, YcbcrConfig* config, std::string* error) {
    const size_t color_end = offsetof(KopawFrame, color) + sizeof(frame->color);
    if (frame->struct_size < color_end) {
        return fail(error, "YCbCr frame lacks explicit color metadata");
    }
    switch (frame->color.range) {
        case KOPAW_COLOR_RANGE_LIMITED:
            config->range = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
            break;
        case KOPAW_COLOR_RANGE_FULL:
            config->range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            break;
        default:
            return fail(error, "YCbCr frame has unknown color range");
    }
    switch (frame->color.matrix) {
        case KOPAW_COLOR_MATRIX_BT601:
            config->model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
            break;
        case KOPAW_COLOR_MATRIX_BT709:
            config->model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            break;
        case KOPAW_COLOR_MATRIX_BT2020_NCL:
            config->model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
            break;
        case KOPAW_COLOR_MATRIX_BT2020_CL:
            return fail(error,
                        "BT.2020 constant-luminance needs a shader conversion");
        default:
            return fail(error, "YCbCr frame has unknown color matrix");
    }

    // Vulkan represents horizontal and vertical chroma sites independently.
    // An unspecified site keeps the historical midpoint sampler choice, but
    // never changes the matrix or range selected above.
    switch (frame->color.chroma_location) {
        case KOPAW_CHROMA_LOCATION_LEFT:
            config->x_chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
            config->y_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
            break;
        case KOPAW_CHROMA_LOCATION_TOPLEFT:
            config->x_chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
            config->y_chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
            break;
        case KOPAW_CHROMA_LOCATION_TOP:
            config->x_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
            config->y_chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
            break;
        case KOPAW_CHROMA_LOCATION_CENTER:
        case KOPAW_CHROMA_LOCATION_UNKNOWN:
            config->x_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
            config->y_chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
            break;
        case KOPAW_CHROMA_LOCATION_BOTTOMLEFT:
        case KOPAW_CHROMA_LOCATION_BOTTOM:
            return fail(error,
                        "bottom-aligned chroma needs explicit reconstruction");
        default:
            return fail(error, "YCbCr frame has invalid chroma location");
    }
    return true;
}

} // namespace

bool describe_ycbcr_frame(const KopawFrame* frame, YcbcrConfig* config,
                          std::string* error) {
    if (!frame || !config || frame->media_type != KOPAW_MEDIA_VIDEO ||
        frame->memory_type != KOPAW_MEMORY_DMABUF || frame->plane_count != 2) {
        return fail(error, "YCbCr requires a two-plane video DMA-BUF");
    }
    YcbcrConfig result;
    if (frame->drm_fourcc == kDrmFormatNv12) {
        result.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    } else if (frame->drm_fourcc == kDrmFormatP010) {
        result.format = VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    } else {
        return fail(error, "YCbCr supports only NV12 and P010");
    }
    const auto& video = frame->format.video;
    if (!video.width || !video.height || (video.width & 1) ||
        (video.height & 1)) {
        return fail(error, "YCbCr 4:2:0 requires nonzero even dimensions");
    }
    if (!frame->retain || !frame->release) {
        return fail(error, "YCbCr requires reference-counted frame ownership");
    }
    if (frame->acquire_fence.kind != KOPAW_SYNC_FENCE_NONE) {
        return fail(error, "YCbCr requires producer-synchronized frames");
    }
    const auto& y = frame->planes[0];
    const auto& uv = frame->planes[1];
    if (!same_object(y.fd, uv.fd)) {
        return fail(error, "YCbCr requires both planes in one DMA-BUF object");
    }
    if (y.modifier == kDrmFormatModInvalid || y.modifier != uv.modifier) {
        return fail(error,
                    "YCbCr requires one explicit DRM modifier for both planes");
    }
    const uint64_t row_bytes =
        uint64_t(video.width) * (frame->drm_fourcc == kDrmFormatP010 ? 2 : 1);
    if (y.stride < row_bytes || uv.stride < row_bytes) {
        return fail(error, "YCbCr plane stride is smaller than a pixel row");
    }
    if (y.modifier == kDrmFormatModLinear) {
        const uint64_t y_end = uint64_t(y.offset) +
                               uint64_t(y.stride) * (video.height - 1) +
                               row_bytes;
        const uint64_t uv_end = uint64_t(uv.offset) +
                                uint64_t(uv.stride) * (video.height / 2 - 1) +
                                row_bytes;
        if (uv.offset < y_end ||
            (frame->size && (y_end > frame->size || uv_end > frame->size))) {
            return fail(
                error,
                "YCbCr linear plane layout overlaps or exceeds the buffer");
        }
    }
    result.modifier = y.modifier;
    if (!map_color(frame, &result, error)) return false;
    *config = result;
    return true;
}

bool query_ycbcr_support(VkPhysicalDevice physical, uint32_t width,
                         uint32_t height, YcbcrConfig* config,
                         std::string* error) {
    if (!config) return fail(error, "Missing YCbCr configuration");
    VkDrmFormatModifierPropertiesListEXT modifiers{};
    modifiers.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    props.pNext = &modifiers;
    vkGetPhysicalDeviceFormatProperties2(physical, config->format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(
        modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = entries.data();
    vkGetPhysicalDeviceFormatProperties2(physical, config->format, &props);
    VkFormatFeatureFlags features = 0;
    for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; ++i) {
        if (entries[i].drmFormatModifier == config->modifier &&
            entries[i].drmFormatModifierPlaneCount == 2) {
            features = entries[i].drmFormatModifierTilingFeatures;
            break;
        }
    }
    if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return fail(error,
                    "YCbCr modifier does not support two-plane sampling");
    }
    const auto supports_chroma = [features](VkChromaLocation location) {
        return location == VK_CHROMA_LOCATION_MIDPOINT
                   ? (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) != 0
                   : (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) != 0;
    };
    if (!supports_chroma(config->x_chroma_location) ||
        !supports_chroma(config->y_chroma_location)) {
        return fail(error,
                    "YCbCr modifier does not support the frame chroma location");
    }
    constexpr VkFormatFeatureFlags linear =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
    config->filter =
        (features & linear) == linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{};
    modifier.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
    modifier.drmFormatModifier = config->modifier;
    modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo external{};
    external.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.pNext = &modifier;
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.pNext = &external;
    info.format = config->format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkSamplerYcbcrConversionImageFormatProperties ycbcr{};
    ycbcr.sType =
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
    VkExternalImageFormatProperties imported{};
    imported.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    imported.pNext = &ycbcr;
    VkImageFormatProperties2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    supported.pNext = &imported;
    const VkResult status =
        vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &supported);
    if (status != VK_SUCCESS ||
        !(imported.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(imported.externalMemoryProperties.compatibleHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
        !ycbcr.combinedImageSamplerDescriptorCount) {
        return fail(error,
                    "YCbCr format/modifier cannot be imported as a DMA-BUF");
    }
    if (width > supported.imageFormatProperties.maxExtent.width ||
        height > supported.imageFormatProperties.maxExtent.height) {
        return fail(error, "YCbCr dimensions exceed the device format limits");
    }
    config->descriptor_count = ycbcr.combinedImageSamplerDescriptorCount;
    config->max_extent = {supported.imageFormatProperties.maxExtent.width,
                          supported.imageFormatProperties.maxExtent.height};
    return true;
}

} // namespace kopaw
