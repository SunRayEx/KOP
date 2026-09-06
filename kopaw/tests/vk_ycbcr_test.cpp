#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "frame.hpp"
#include "render/vulkan/vk_ycbcr.hpp"

namespace {

void check(bool ok, const char* expression, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

constexpr VkFormatFeatureFlags kSampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
constexpr VkFormatFeatureFlags kMidpoint =
    VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
constexpr VkFormatFeatureFlags kLinear =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
VkFormatFeatureFlags features = kSampled | kMidpoint | kLinear;
uint64_t modifier_value = 0;
uint32_t plane_count = 2;
uint32_t descriptor_count = 3;
bool importable = true;
VkResult query_status = VK_SUCCESS;
unsigned queries = 0;

KopawFrame make_frame(int fd) {
    KopawFrame frame{};
    frame.media_type = KOPAW_MEDIA_VIDEO;
    frame.memory_type = KOPAW_MEMORY_DMABUF;
    frame.drm_fourcc = kopaw::kDrmFormatNv12;
    frame.format.video.width = 1280;
    frame.format.video.height = 720;
    frame.plane_count = 2;
    frame.planes[0].fd = frame.planes[1].fd = fd;
    frame.planes[0].stride = frame.planes[1].stride = 1280;
    frame.planes[1].offset = 1280 * 720;
    frame.size = 1280 * 1080;
    frame.retain = [](KopawFrame*) {};
    frame.release = [](KopawFrame*) {};
    return frame;
}

} // namespace

// Simulated drivers deliberately expose different optimal/modifier features,
// so these tests exercise capability decisions without requiring a GPU.
extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice, VkFormat format, VkFormatProperties2* properties) {
    CHECK(properties->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2);
    auto* list =
        static_cast<VkDrmFormatModifierPropertiesListEXT*>(properties->pNext);
    CHECK(list->sType ==
          VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
    properties->formatProperties.optimalTilingFeatures =
        kSampled | kMidpoint | kLinear;
    if (format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
        list->drmFormatModifierCount = 0;
        return;
    }
    if (list->pDrmFormatModifierProperties) {
        CHECK(list->drmFormatModifierCount >= 1);
        list->pDrmFormatModifierProperties[0] = {modifier_value, plane_count,
                                                 features};
    }
    list->drmFormatModifierCount = 1;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2* info,
    VkImageFormatProperties2* properties) {
    ++queries;
    CHECK(info->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
    CHECK(info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
    CHECK(info->usage == VK_IMAGE_USAGE_SAMPLED_BIT);
    auto* external =
        static_cast<const VkPhysicalDeviceExternalImageFormatInfo*>(
            info->pNext);
    CHECK(external->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    CHECK(external->handleType ==
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    auto* modifier =
        static_cast<const VkPhysicalDeviceImageDrmFormatModifierInfoEXT*>(
            external->pNext);
    CHECK(modifier->sType ==
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
    CHECK(modifier->drmFormatModifier == modifier_value);
    CHECK(modifier->sharingMode == VK_SHARING_MODE_EXCLUSIVE);
    auto* imported =
        static_cast<VkExternalImageFormatProperties*>(properties->pNext);
    CHECK(imported->sType ==
          VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    auto* ycbcr = static_cast<VkSamplerYcbcrConversionImageFormatProperties*>(
        imported->pNext);
    CHECK(ycbcr->sType ==
          VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES);
    imported->externalMemoryProperties.externalMemoryFeatures =
        importable ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0;
    imported->externalMemoryProperties.compatibleHandleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ycbcr->combinedImageSamplerDescriptorCount = descriptor_count;
    properties->imageFormatProperties.maxExtent = {4096, 4096, 1};
    return query_status;
}

int main() {
    FILE* object = std::tmpfile();
    FILE* other = std::tmpfile();
    CHECK(object && other);
    auto frame = make_frame(fileno(object));
    kopaw::YcbcrConfig config;
    std::string error;
    CHECK(kopaw::describe_ycbcr_frame(&frame, &config, &error));
    CHECK(config.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
    CHECK(config.model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709);
    CHECK(
        kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config, &error));
    CHECK(config.descriptor_count == 3);
    CHECK(config.filter == VK_FILTER_LINEAR);
    CHECK(config.chroma_location == VK_CHROMA_LOCATION_MIDPOINT);

    frame.format.video.height = 480;
    CHECK(kopaw::describe_ycbcr_frame(&frame, &config, &error));
    CHECK(config.model == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601);
    frame = make_frame(fileno(object));
    frame.drm_fourcc = kopaw::kDrmFormatP010;
    frame.planes[0].stride = frame.planes[1].stride = 2560;
    frame.planes[1].offset *= 2;
    frame.size *= 2;
    CHECK(kopaw::describe_ycbcr_frame(&frame, &config, &error));
    CHECK(config.format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16);
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    frame = make_frame(fileno(object));
    CHECK(kopaw::describe_ycbcr_frame(&frame, &config, &error));
    CHECK(
        kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config, &error));

    features = kSampled | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
    CHECK(
        kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config, &error));
    CHECK(config.chroma_location == VK_CHROMA_LOCATION_COSITED_EVEN);
    CHECK(config.filter == VK_FILTER_NEAREST);
    features = kSampled | kMidpoint |
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    CHECK(
        kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config, &error));
    CHECK(config.filter == VK_FILTER_NEAREST);
    features = kSampled;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    features = kMidpoint;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    features = kSampled | kMidpoint;
    plane_count = 3;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    plane_count = 2;
    importable = false;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    importable = true;
    descriptor_count = 0;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    descriptor_count = 3;
    query_status = VK_ERROR_FORMAT_NOT_SUPPORTED;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    query_status = VK_SUCCESS;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 8192, 720, &config,
                                      &error));
    modifier_value = 123;
    CHECK(!kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config,
                                      &error));
    config.modifier = 123;
    CHECK(
        kopaw::query_ycbcr_support(VK_NULL_HANDLE, 1280, 720, &config, &error));
    CHECK(queries > 0);

    auto invalid = frame;
    invalid.planes[1].fd = fileno(other);
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid.planes[1].fd = dup(frame.planes[0].fd);
    CHECK(invalid.planes[1].fd >= 0);
    CHECK(kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    close(invalid.planes[1].fd);
    invalid = frame;
    invalid.planes[1].offset = 1;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.planes[1].stride = 1;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.size -= 1;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.planes[1].modifier = 123;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid.planes[0].modifier = invalid.planes[1].modifier =
        kopaw::kDrmFormatModInvalid;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.format.video.width = 1279;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.plane_count = 3;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.retain = nullptr;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    invalid = frame;
    invalid.drm_fourcc = 0;
    CHECK(!kopaw::describe_ycbcr_frame(&invalid, &config, &error));
    CHECK(!kopaw::describe_ycbcr_frame(nullptr, &config, &error));
    std::fclose(object);
    std::fclose(other);
    std::puts(
        "YCbCr format, modifier, descriptor and frame-layout checks passed");
}
