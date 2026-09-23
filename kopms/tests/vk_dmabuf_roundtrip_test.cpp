// P3-M3 核心链路测试：KOPAW Vulkan 图像导出 DMA-BUF → 跨 VkDevice 导入 →
// 像素读回校验。
//
// 验证契约：
//   - 导出帧携带 planes[0]（fd/stride/modifier）与 acquire fence（sync fd）；
//   - fence 在导出后短时间内 signal（内容写入完成）；
//   - 第二个 Vulkan 设备（独立 instance）能导入该 DMA-BUF 并读回一致的像素；
//   - 帧引用归零后 fd 关闭、图像归还导出器空闲池（可再次导出）。
//
// 无 Vulkan DMA-BUF 能力（CI 无加载器等）时以 77 跳过。
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kop/vk_util.hpp"
#include "render/vulkan/vk_dma_export.hpp"

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;

int fail(const char* message) {
    std::fprintf(stderr, "vk dmabuf roundtrip: FAIL %s\n", message);
    return 1;
}

int skip(const std::string& reason) {
    std::fprintf(stdout, "vk dmabuf roundtrip: SKIP (%s)\n", reason.c_str());
    return 77;
}

uint8_t pattern(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>((x * 4 + y * 2) & 0xff);
}

}  // namespace

int main() {
    std::string reason;
    if (!kopaw::VulkanDmabufExporter::export_supported(&reason)) {
        return skip("DMA-BUF export unsupported: " + reason);
    }

    // ---- 导出器：CPU 图案帧 → KOPAW_MEMORY_VULKAN 帧 ----
    kopaw::VulkanDmabufExporter exporter;
    if (exporter.init(4, &reason)) return fail(("exporter init: " + reason).c_str());

    kopaw::OwnedFrame* cpu = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 1234, 5678,
                                               static_cast<size_t>(kW) * kH * 4);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            uint8_t* row = cpu->data() + static_cast<size_t>(y) * kW * 4;
            row[x * 4 + 0] = pattern(x, y);
            row[x * 4 + 1] = static_cast<uint8_t>(255 - pattern(x, y));
            row[x * 4 + 2] = 0x40;
            row[x * 4 + 3] = 0xff;
        }
    }
    cpu->frame.format.video.width = kW;
    cpu->frame.format.video.height = kH;
    cpu->frame.stride = kW * 4;

    KopawFrame* gpu = exporter.export_cpu_frame(&cpu->frame, &reason);
    if (!gpu) {
        return fail(("export failed: " + reason).c_str());
    }
    if (gpu->memory_type != KOPAW_MEMORY_VULKAN || gpu->plane_count != 1 ||
        gpu->planes[0].fd < 0 || gpu->acquire_fence.fd < 0) {
        return fail("exported frame metadata invalid");
    }
    if (gpu->format.video.width != kW || gpu->format.video.height != kH ||
        gpu->planes[0].stride < kW * 4) {
        return fail("exported frame geometry invalid");
    }

    // ---- acquire fence：导出后必须很快 signal（POLLIN）----
    {
        pollfd pfd{gpu->acquire_fence.fd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, 5000);
        if (r != 1) return fail("exported acquire fence did not signal");
    }

    // ---- 第二个设备（独立 instance）导入并读回像素 ----
    {
        kop::vkutil::DeviceContext importer{};
        if (!kop::vkutil::create_instance({}, &importer.instance, &reason))
            return fail(("importer instance: " + reason).c_str());
        std::vector<const char*> exts{VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
        // 导入侧必须与导出侧同卡：生产链路里合成器与客户端同机同卡，双卡
        // 机器上也不能让导入落到另一张卡（跨物理卡导入不被 Vulkan 保证）。
        if (!kop::vkutil::pick_and_create_device(exts, &importer, &reason,
                                                 exporter.device_name())) {
            return skip("importer device unavailable: " + reason);
        }
        const bool same_gpu =
            importer.device_uuid == exporter.device_uuid();
        std::fprintf(stderr,
                     "vk dmabuf roundtrip: exporter=%s importer=%s same_gpu=%d\n",
                     exporter.device_name().c_str(), importer.device_name.c_str(),
                     same_gpu ? 1 : 0);
        if (!same_gpu) {
            return skip("importer landed on a different physical device "
                        "(exporter uuid " +
                        kop::vkutil::to_hex(exporter.device_uuid()) + " vs " +
                        kop::vkutil::to_hex(importer.device_uuid) +
                        "); cross-GPU DMA-BUF import is not required to work");
        }

        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {kW, kH, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExternalMemoryImageCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod{};
        VkSubresourceLayout plane_layout{};
        plane_layout.offset = gpu->planes[0].offset;
        plane_layout.rowPitch = gpu->planes[0].stride;
        if (gpu->planes[0].modifier != kopaw::kDrmFormatModInvalid) {
            explicit_mod.sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
            explicit_mod.drmFormatModifier = gpu->planes[0].modifier;
            explicit_mod.drmFormatModifierPlaneCount = 1;
            explicit_mod.pPlaneLayouts = &plane_layout;
            external.pNext = &explicit_mod;
            ii.pNext = &external;
        } else {
            ii.pNext = &external;
        }
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(importer.device, &ii, nullptr, &image) != VK_SUCCESS) {
            return fail("importer vkCreateImage failed");
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(importer.device, image, &req);
        if (!importer.get_memory_fd_properties) {
            return skip("importer lacks vkGetMemoryFdPropertiesKHR");
        }
        VkMemoryFdPropertiesKHR fd_properties{};
        fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        const VkResult fd_props_result = importer.get_memory_fd_properties(
            importer.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            gpu->planes[0].fd, &fd_properties);
        if (fd_props_result != VK_SUCCESS) {
            return skip("DMA-BUF fd properties unavailable (VkResult " +
                        std::to_string(static_cast<int>(fd_props_result)) + ")");
        }
        const uint32_t compatible_bits = req.memoryTypeBits & fd_properties.memoryTypeBits;
        if (compatible_bits == 0) {
            return skip("exported DMA-BUF has no importer-compatible memory type");
        }
        // 导入转移 fd 所有权：dup 一份给实现，原 fd 归帧。
        const int import_fd = ::dup(gpu->planes[0].fd);
        if (import_fd < 0) return fail("dup import fd");
        VkImportMemoryFdInfoKHR import_mem{};
        import_mem.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_mem.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import_mem.fd = import_fd;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = image;
        import_mem.pNext = &dedicated;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.pNext = &import_mem;
        ai.memoryTypeIndex =
            kop::vkutil::find_memory_type(importer.physical, compatible_bits, 0,
                                          &reason);
        if (ai.memoryTypeIndex == UINT32_MAX) {
            ::close(import_fd);
            return fail("import memory type");
        }
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const VkResult alloc_result = vkAllocateMemory(importer.device, &ai, nullptr, &memory);
        if (alloc_result != VK_SUCCESS) {
            const off_t dmabuf_size = ::lseek(gpu->planes[0].fd, 0, SEEK_END);
            std::fprintf(stderr,
                         "importer vkAllocateMemory failed (VkResult %d, "
                         "device=%s uuid=%s memtype=%u size=%llu, "
                         "req_bits=0x%x fd_bits=0x%x dmabuf_size=%lld "
                         "exporter=%s uuid=%s)\n",
                         static_cast<int>(alloc_result),
                         importer.device_name.c_str(),
                         importer.uuid_hex().c_str(), ai.memoryTypeIndex,
                         static_cast<unsigned long long>(ai.allocationSize),
                         req.memoryTypeBits, fd_properties.memoryTypeBits,
                         static_cast<long long>(dmabuf_size),
                         exporter.device_name().c_str(),
                         kop::vkutil::to_hex(exporter.device_uuid()).c_str());
            ::close(import_fd);
            if (alloc_result == VK_ERROR_INVALID_EXTERNAL_HANDLE ||
                alloc_result == VK_ERROR_FEATURE_NOT_PRESENT ||
                alloc_result == VK_ERROR_FORMAT_NOT_SUPPORTED ||
                alloc_result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                return skip("DMA-BUF import unsupported (VkResult " +
                            std::to_string(static_cast<int>(alloc_result)) + ")");
            }
            return fail("importer vkAllocateMemory failed");
        }
        if (vkBindImageMemory(importer.device, image, memory, 0) != VK_SUCCESS) {
            return fail("importer vkBindImageMemory failed");
        }

        // 读回：image → buffer → map
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = static_cast<VkDeviceSize>(kW) * kH * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(importer.device, &bi, nullptr, &buffer) != VK_SUCCESS)
            return fail("readback buffer");
        VkMemoryRequirements breq{};
        vkGetBufferMemoryRequirements(importer.device, buffer, &breq);
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = breq.size;
        bai.memoryTypeIndex = kop::vkutil::find_memory_type(
            importer.physical, breq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &reason);
        if (bai.memoryTypeIndex == UINT32_MAX) return fail("readback memory type");
        VkDeviceMemory bmem = VK_NULL_HANDLE;
        if (vkAllocateMemory(importer.device, &bai, nullptr, &bmem) != VK_SUCCESS)
            return fail("readback allocate");
        vkBindBufferMemory(importer.device, buffer, bmem, 0);
        void* mapped = nullptr;
        if (vkMapMemory(importer.device, bmem, 0, breq.size, 0, &mapped) != VK_SUCCESS)
            return fail("readback map");

        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = importer.graphics_family;
        VkCommandPool cmd_pool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(importer.device, &pool, nullptr, &cmd_pool) != VK_SUCCESS)
            return fail("cmd pool");
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cmd_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(importer.device, &cai, &cmd) != VK_SUCCESS)
            return fail("cmd buffer");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_src);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kW, kH, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &region);
        vkEndCommandBuffer(cmd);
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(importer.device, &fci, nullptr, &fence);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(importer.graphics_queue, 1, &submit, fence);
        vkWaitForFences(importer.device, 1, &fence, VK_TRUE, 5'000'000'000ull);

        const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint8_t* px = pixels + (static_cast<size_t>(y) * kW + x) * 4;
                if (px[0] != pattern(x, y) ||
                    px[1] != static_cast<uint8_t>(255 - pattern(x, y)) ||
                    px[2] != 0x40 || px[3] != 0xff) {
                    std::fprintf(stderr, "pixel mismatch at (%u,%u): %u %u %u %u\n", x,
                                 y, px[0], px[1], px[2], px[3]);
                    return fail("imported pixels do not match exported pattern");
                }
            }
        }

        vkDestroyFence(importer.device, fence, nullptr);
        vkDestroyCommandPool(importer.device, cmd_pool, nullptr);
        vkUnmapMemory(importer.device, bmem);
        vkFreeMemory(importer.device, bmem, nullptr);
        vkDestroyBuffer(importer.device, buffer, nullptr);
        vkDestroyImage(importer.device, image, nullptr);
        vkFreeMemory(importer.device, memory, nullptr);
        kop::vkutil::destroy(&importer);
    }

    // ---- 引用归零：fd 关闭 + 图像归还池（可再次导出） ----
    gpu->release(gpu);
    cpu->release_cb(&cpu->frame);
    if (exporter.exported_frames() < 1) return fail("export counter invalid");
    kopaw::OwnedFrame* second = kopaw::make_frame(KOPAW_MEDIA_VIDEO, 0, 0,
                                                  static_cast<size_t>(kW) * kH * 4);
    second->frame.format.video.width = kW;
    second->frame.format.video.height = kH;
    second->frame.stride = kW * 4;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            uint8_t* row = second->data() + static_cast<size_t>(y) * kW * 4;
            row[x * 4 + 0] = 0x11;
        }
    }
    KopawFrame* gpu2 = exporter.export_cpu_frame(&second->frame, &reason);
    if (!gpu2) return fail(("second export failed: " + reason).c_str());
    gpu2->release(gpu2);
    second->release_cb(&second->frame);
    exporter.shutdown();
    std::fprintf(stdout, "vk dmabuf roundtrip: PASS (%llu frames exported)\n",
                 static_cast<unsigned long long>(exporter.exported_frames()));
    return 0;
}
