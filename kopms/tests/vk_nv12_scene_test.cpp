// P2 核心链路测试：VulkanScene NV12 两平面导入 → YUV→RGB 合成 → 目标回读。
//
// 生产端在本进程用 Vulkan 导出 NV12 LINEAR DMA-BUF（等价 VAAPI 解码帧的
// 交付形态：两平面共享同一 DRM 对象、双平面 stride/offset、无 acquire
// fence——解码同步已在生产侧完成）；VulkanScene 以 BUS 同款路径导入、
// 合成、导出目标；读回端逐点核对 BT.601 有限范围转换结果：
//   Y=64,  U=V=128 → RGB ≈ (64-16)*1.164  = 56
//   Y=200, U=V=128 → RGB ≈ (200-16)*1.164 = 214
// 无 DMA-BUF 能力的环境以 77 跳过。
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kop/vk_util.hpp"
#include "vk_scene.hpp"

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;
constexpr uint32_t kNv12Fourcc = 0x3231564Eu;  // DRM_FORMAT_NV12

int fail(const char* message) {
    std::fprintf(stderr, "vk nv12 scene: FAIL %s\n", message);
    return 1;
}

int skip(const std::string& reason) {
    std::fprintf(stdout, "vk nv12 scene: SKIP (%s)\n", reason.c_str());
    return 77;
}

// 参考实现（与 kopaw/modules/render/shaders/color.glsl 同一公式）：
// 非线性编码值 → EOTF 线性化 →（PQ/HLG 色调映射）→ sRGB OETF 编码。
// 8bit 有限范围展开：(Y-16)/219，色度为中性时非线性 RGB == Y。
float eotf_bt709(float v) {
    return v < 0.081f ? v / 4.5f
                     : std::pow((v + 0.099f) / 1.099f, 1.0f / 0.45f);
}

float eotf_pq(float v) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    const float em = std::pow(v, 1.0f / m2);
    return 10000.0f * std::pow((em - c1) / (c2 - c3 * em), 1.0f / m1);
}

float tonemap(float lin, float peak_in) {
    const float t = lin / peak_in;
    const float scale = (t <= 1.0f ? 1.0f : (2.0f - 1.0f / t) / t) / peak_in;
    return lin * scale;
}

float oetf_srgb(float l) {
    return l <= 0.0031308f ? l * 12.92f
                           : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

// 有限范围 luma 编码值 → 期望的 8bit 输出（0-255 浮点，BT.709 传递函数）
float expect_sdr(uint8_t y) {
    return oetf_srgb(eotf_bt709((y - 16.0f) / 219.0f)) * 255.0f;
}

// 有限范围 luma 编码值 → PQ 解码 + 以 max_cll 为锚的色调映射 → sRGB 编码
float expect_pq(uint8_t y, float max_cll) {
    return oetf_srgb(tonemap(eotf_pq((y - 16.0f) / 219.0f), max_cll)) * 255.0f;
}

}  // namespace

int main() {
    std::string reason;

    // ---- 生产端：导出 NV12 LINEAR DMA-BUF（两平面同一对象）----
    kop::vkutil::DeviceContext producer{};
    if (!kop::vkutil::create_instance({}, &producer.instance, &reason)) {
        return fail(("producer instance: " + reason).c_str());
    }
    std::vector<const char*> prod_exts{VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
    if (!kop::vkutil::pick_and_create_device(prod_exts, &producer, &reason)) {
        return skip("producer device unavailable: " + reason);
    }

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    ii.extent = {kW, kH, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageDrmFormatModifierListCreateInfoEXT mod_list{};
    const uint64_t linear = 0;  // DRM_FORMAT_MOD_LINEAR
    mod_list.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    mod_list.drmFormatModifierCount = 1;
    mod_list.pDrmFormatModifiers = &linear;
    external.pNext = &mod_list;
    ii.pNext = &external;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(producer.device, &ii, nullptr, &image) != VK_SUCCESS) {
        return skip("NV12 external image unsupported on this driver");
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(producer.device, image, &req);
    VkExportMemoryAllocateInfo export_mem{};
    export_mem.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_mem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    export_mem.pNext = &dedicated;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.pNext = &export_mem;
    ai.memoryTypeIndex = kop::vkutil::find_memory_type(
        producer.physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &reason);
    if (ai.memoryTypeIndex == UINT32_MAX) return fail("producer memory type");
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(producer.device, &ai, nullptr, &memory) != VK_SUCCESS) {
        return fail("producer vkAllocateMemory");
    }
    if (vkBindImageMemory(producer.device, image, memory, 0) != VK_SUCCESS) {
        return fail("producer vkBindImageMemory");
    }

    // 平面布局（LINEAR：stride/offset 由驱动查询）
    VkSubresourceLayout plane_layout[2]{};
    const VkImageAspectFlagBits aspects[2] = {VK_IMAGE_ASPECT_PLANE_0_BIT,
                                              VK_IMAGE_ASPECT_PLANE_1_BIT};
    for (int p = 0; p < 2; ++p) {
        VkImageSubresource sub{aspects[p], 0, 0};
        vkGetImageSubresourceLayout(producer.device, image, &sub, &plane_layout[p]);
    }
    const VkDeviceSize y_bytes = plane_layout[0].rowPitch * kH;
    const VkDeviceSize uv_bytes = plane_layout[1].rowPitch * (kH / 2);

    // staging：左半 Y=64（暗），右半 Y=200（亮）；UV 恒 128（中性色度）
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = y_bytes + uv_bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer stage = VK_NULL_HANDLE;
    if (vkCreateBuffer(producer.device, &bi, nullptr, &stage) != VK_SUCCESS) {
        return fail("producer staging buffer");
    }
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(producer.device, stage, &sreq);
    VkMemoryAllocateInfo sai{};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = kop::vkutil::find_memory_type(
        producer.physical, sreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &reason);
    if (sai.memoryTypeIndex == UINT32_MAX) return fail("staging memory type");
    VkDeviceMemory smem = VK_NULL_HANDLE;
    if (vkAllocateMemory(producer.device, &sai, nullptr, &smem) != VK_SUCCESS) {
        return fail("staging allocate");
    }
    vkBindBufferMemory(producer.device, stage, smem, 0);
    void* mapped = nullptr;
    if (vkMapMemory(producer.device, smem, 0, sreq.size, 0, &mapped) != VK_SUCCESS) {
        return fail("staging map");
    }
    auto* bytes = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            bytes[y * plane_layout[0].rowPitch + x] = (x < kW / 2) ? 64 : 200;
        }
    }
    for (VkDeviceSize i = 0; i < uv_bytes; ++i) {
        bytes[y_bytes + i] = 128;
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = producer.graphics_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(producer.device, &cpi, nullptr, &pool) != VK_SUCCESS) {
        return fail("producer cmd pool");
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(producer.device, &cai, &cmd) != VK_SUCCESS) {
        return fail("producer cmd buffer");
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_dst);
    VkBufferImageCopy regions[2]{};
    regions[0].bufferOffset = 0;
    regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    regions[0].imageExtent = {kW, kH, 1};
    regions[1].bufferOffset = y_bytes;
    regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    regions[1].imageExtent = {kW / 2, kH / 2, 1};
    vkCmdCopyBufferToImage(cmd, stage, image, VK_IMAGE_LAYOUT_GENERAL, 2, regions);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(producer.device, &fci, nullptr, &fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(producer.graphics_queue, 1, &submit, fence);
    vkWaitForFences(producer.device, 1, &fence, VK_TRUE, 5'000'000'000ull);
    vkDestroyFence(producer.device, fence, nullptr);

    // ---- 自检：把写入后的 NV12 图像读回，确认生产端内容正确 ----
    {
        VkCommandBufferBeginInfo vbegin{};
        vbegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vbegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &vbegin);
        VkBufferImageCopy vregions[2]{};
        vregions[0].bufferOffset = 0;
        vregions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        vregions[0].imageExtent = {kW, kH, 1};
        vregions[1].bufferOffset = y_bytes;
        vregions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
        vregions[1].imageExtent = {kW / 2, kH / 2, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, stage, 2,
                               vregions);
        vkEndCommandBuffer(cmd);
        VkFence vfence = VK_NULL_HANDLE;
        vkCreateFence(producer.device, &fci, nullptr, &vfence);
        VkSubmitInfo vsubmit{};
        vsubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        vsubmit.commandBufferCount = 1;
        vsubmit.pCommandBuffers = &cmd;
        vkQueueSubmit(producer.graphics_queue, 1, &vsubmit, vfence);
        vkWaitForFences(producer.device, 1, &vfence, VK_TRUE, 5'000'000'000ull);
        vkDestroyFence(producer.device, vfence, nullptr);
        if (bytes[0] != 64 || bytes[40] != 200 ||
            bytes[y_bytes] != 128) {
            return fail("producer NV12 content check");
        }
    }

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int nv12_fd = -1;
    if (producer.get_memory_fd(producer.device, &fd_info, &nv12_fd) != VK_SUCCESS ||
        nv12_fd < 0) {
        return fail("producer export fd");
    }

    // ---- 场景：BUS 同款导入/合成路径 ----
    kopms::VulkanScene scene;
    kopms::VulkanScene::Options opts{};
    opts.windowed = false;
    opts.width = kW;
    opts.height = kH;
    if (scene.init(opts, nullptr, &reason)) {
        return fail(("scene init: " + reason).c_str());
    }
    uint32_t released = 0;
    scene.set_release_callback(
        [&](uint64_t, uint64_t, uint32_t) { ++released; });
    const uint32_t expected_releases = 2;

    kopms::VulkanScene::ImportRequest request{};
    request.width = kW;
    request.height = kH;
    request.format = kNv12Fourcc;
    request.plane_count = 2;
    int plane_fds[2] = {nv12_fd, nv12_fd};  // 同一 DRM 对象（去重后的接收形态）
    uint32_t offsets[2] = {static_cast<uint32_t>(plane_layout[0].offset),
                           static_cast<uint32_t>(plane_layout[1].offset)};
    uint32_t strides[2] = {static_cast<uint32_t>(plane_layout[0].rowPitch),
                           static_cast<uint32_t>(plane_layout[1].rowPitch)};
    uint64_t modifiers[2] = {linear, linear};
    request.plane_fds = plane_fds;
    request.plane_offsets = offsets;
    request.plane_strides = strides;
    request.plane_modifiers = modifiers;
    request.acquire_fence_kind = KOPAW_SYNC_FENCE_NONE;
    request.has_color_metadata = true;
    request.color.range = KOPAW_COLOR_RANGE_LIMITED;
    request.color.matrix = KOPAW_COLOR_MATRIX_BT601;
    request.color.primaries = KOPAW_COLOR_PRIMARIES_BT601;
    request.color.chroma_location = KOPAW_CHROMA_LOCATION_CENTER;

    // 两个窗口并排，导入同一张 NV12，但走不同传递函数：左窗 SDR（BT.709
    // 直上 sRGB），右窗 HDR（PQ + MaxCLL=2000 做色调映射）。
    request.color.transfer = KOPAW_COLOR_TRANSFER_BT709;
    request.session_id = 1;
    request.frame_id = 1;
    if (!scene.submit(1, request, &reason)) {
        return fail(("scene submit sdr: " + reason).c_str());
    }

    request.color.transfer = KOPAW_COLOR_TRANSFER_PQ;
    request.color.hdr.flags = KOPAW_HDR_FLAG_CONTENT_LIGHT;
    request.color.hdr.max_cll = 4000;  // cd/m²，色调映射锚点
    request.session_id = 2;
    request.frame_id = 2;
    if (!scene.submit(2, request, &reason)) {
        return fail(("scene submit pq: " + reason).c_str());
    }

    const int32_t half = static_cast<int32_t>(kW / 2);
    std::vector<kopms::VulkanScene::LayoutItem> layout(2);
    layout[0].window_id = 1;
    layout[0].z = 0;
    layout[0].rect = {0, 0, half, static_cast<int32_t>(kH)};
    layout[1].window_id = 2;
    layout[1].z = 0;
    layout[1].rect = {half, 0, half, static_cast<int32_t>(kH)};
    if (!scene.render(layout, &reason)) {
        return fail(("scene render: " + reason).c_str());
    }

    // 导出目标（内部等待合成 fence）→ 读回端导入
    int target_fd = -1;
    uint32_t target_stride = 0, target_w = 0, target_h = 0;
    if (!scene.export_composite_dmabuf(&target_fd, &target_stride, &target_w,
                                       &target_h, &reason)) {
        return fail(("export composite: " + reason).c_str());
    }

    // ---- 读回端：独立设备导入目标并逐点核对 YUV→RGB ----
    {
        kop::vkutil::DeviceContext reader{};
        if (!kop::vkutil::create_instance({}, &reader.instance, &reason)) {
            return fail(("reader instance: " + reason).c_str());
        }
        std::vector<const char*> r_exts{VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
        if (!kop::vkutil::pick_and_create_device(r_exts, &reader, &reason)) {
            return skip("reader device unavailable: " + reason);
        }
        VkImageCreateInfo ti{};
        ti.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ti.imageType = VK_IMAGE_TYPE_2D;
        ti.format = VK_FORMAT_B8G8R8A8_UNORM;
        ti.extent = {target_w, target_h, 1};
        ti.mipLevels = 1;
        ti.arrayLayers = 1;
        ti.samples = VK_SAMPLE_COUNT_1_BIT;
        ti.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ti.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ti.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExternalMemoryImageCreateInfo texternal{};
        texternal.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        texternal.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageDrmFormatModifierExplicitCreateInfoEXT tmod{};
        VkSubresourceLayout tlayout{};
        tlayout.rowPitch = target_stride;
        tmod.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        tmod.drmFormatModifier = linear;
        tmod.drmFormatModifierPlaneCount = 1;
        tmod.pPlaneLayouts = &tlayout;
        texternal.pNext = &tmod;
        ti.pNext = &texternal;
        VkImage timage = VK_NULL_HANDLE;
        if (vkCreateImage(reader.device, &ti, nullptr, &timage) != VK_SUCCESS) {
            return fail("reader vkCreateImage(target)");
        }
        VkMemoryRequirements treq{};
        vkGetImageMemoryRequirements(reader.device, timage, &treq);
        const int import_fd = ::dup(target_fd);
        if (import_fd < 0) return fail("dup target fd");
        VkImportMemoryFdInfoKHR import_mem{};
        import_mem.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_mem.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import_mem.fd = import_fd;
        VkMemoryDedicatedAllocateInfo tdedicated{};
        tdedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        tdedicated.image = timage;
        import_mem.pNext = &tdedicated;
        VkMemoryAllocateInfo tai{};
        tai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        tai.allocationSize = treq.size;
        tai.pNext = &import_mem;
        VkMemoryFdPropertiesKHR fd_props{};
        fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (!reader.get_memory_fd_properties ||
            reader.get_memory_fd_properties(
                reader.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                target_fd, &fd_props) != VK_SUCCESS) {
            return fail("reader vkGetMemoryFdPropertiesKHR");
        }
        tai.memoryTypeIndex =
            kop::vkutil::find_memory_type(reader.physical,
                                          treq.memoryTypeBits & fd_props.memoryTypeBits,
                                          0, &reason);
        if (tai.memoryTypeIndex == UINT32_MAX) return fail("reader memory type");
        VkDeviceMemory tmem = VK_NULL_HANDLE;
        if (vkAllocateMemory(reader.device, &tai, nullptr, &tmem) != VK_SUCCESS) {
            return fail("reader vkAllocateMemory");
        }
        if (vkBindImageMemory(reader.device, timage, tmem, 0) != VK_SUCCESS) {
            return fail("reader vkBindImageMemory");
        }

        VkBufferCreateInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        rbi.size = static_cast<VkDeviceSize>(target_stride) * target_h;
        rbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer rbuffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(reader.device, &rbi, nullptr, &rbuffer) != VK_SUCCESS) {
            return fail("reader readback buffer");
        }
        VkMemoryRequirements breq{};
        vkGetBufferMemoryRequirements(reader.device, rbuffer, &breq);
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = breq.size;
        bai.memoryTypeIndex = kop::vkutil::find_memory_type(
            reader.physical, breq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &reason);
        if (bai.memoryTypeIndex == UINT32_MAX) return fail("readback memory type");
        VkDeviceMemory bmem = VK_NULL_HANDLE;
        if (vkAllocateMemory(reader.device, &bai, nullptr, &bmem) != VK_SUCCESS) {
            return fail("readback allocate");
        }
        vkBindBufferMemory(reader.device, rbuffer, bmem, 0);
        void* rmapped = nullptr;
        if (vkMapMemory(reader.device, bmem, 0, breq.size, 0, &rmapped) != VK_SUCCESS) {
            return fail("readback map");
        }

        VkCommandPoolCreateInfo rcpi{};
        rcpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        rcpi.queueFamilyIndex = reader.graphics_family;
        VkCommandPool rpool = VK_NULL_HANDLE;
        vkCreateCommandPool(reader.device, &rcpi, nullptr, &rpool);
        VkCommandBufferAllocateInfo rcai{};
        rcai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        rcai.commandPool = rpool;
        rcai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        rcai.commandBufferCount = 1;
        VkCommandBuffer rcmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(reader.device, &rcai, &rcmd);
        VkCommandBufferBeginInfo rbegin{};
        rbegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        rbegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(rcmd, &rbegin);
        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = timage;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(rcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_src);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {target_w, target_h, 1};
        vkCmdCopyImageToBuffer(rcmd, timage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               rbuffer, 1, &region);
        vkEndCommandBuffer(rcmd);
        VkFence rfence = VK_NULL_HANDLE;
        vkCreateFence(reader.device, &fci, nullptr, &rfence);
        VkSubmitInfo rsubmit{};
        rsubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        rsubmit.commandBufferCount = 1;
        rsubmit.pCommandBuffers = &rcmd;
        vkQueueSubmit(reader.graphics_queue, 1, &rsubmit, rfence);
        vkWaitForFences(reader.device, 1, &rfence, VK_TRUE, 5'000'000'000ull);

        // 采样点远离中线（x=31/32 处 LINEAR 过滤会混合左右亮度）
        struct Probe {
            uint32_t x;
            float expect;
            const char* note;
        };
        // 两窗口并排（各 32 宽），同一张 NV12（左半 Y=64 暗、右半 Y=200 亮）。
        // 每窗取两个探针：一个落在暗半、一个落在亮半，均远离中线与窗口边界。
        // 期望值由上面的参考实现算出（BT.709 直上 sRGB / PQ+MaxCLL 色调映射）。
        const Probe probes[4] = {
            {4,  expect_sdr(64),  "sdr dark"},
            {26, expect_sdr(200), "sdr bright"},
            {36, expect_pq(64, 4000.0f),  "pq dark"},
            {58, expect_pq(200, 4000.0f), "pq bright"},
        };
        const auto* pixels = static_cast<const uint8_t*>(rmapped);
        for (const Probe& p : probes) {
            const uint8_t* px =
                pixels + static_cast<size_t>(24) * target_stride + p.x * 4;
            const float b = px[0], g = px[1], r = px[2];
            std::fprintf(stdout,
                         "vk nv12 scene: probe x=%u (%s) rgb=(%.0f,%.0f,%.0f)"
                         " expect=%.1f\n",
                         p.x, p.note, r, g, b, p.expect);
            if (fabsf(r - p.expect) > 2.0f || fabsf(g - p.expect) > 2.0f ||
                fabsf(b - p.expect) > 2.0f) {
                std::fprintf(stderr,
                             "probe x=%u (%s): got (%.0f,%.0f,%.0f), want %.1f\n",
                             p.x, p.note, r, g, b, p.expect);
                return fail("NV12 色彩管线（YUV→RGB→传递函数）结果超出容差");
            }
        }

        vkDestroyFence(reader.device, rfence, nullptr);
        vkUnmapMemory(reader.device, bmem);
        vkFreeMemory(reader.device, bmem, nullptr);
        vkDestroyBuffer(reader.device, rbuffer, nullptr);
        vkDestroyCommandPool(reader.device, rpool, nullptr);
        vkDestroyImage(reader.device, timage, nullptr);
        vkFreeMemory(reader.device, tmem, nullptr);
        kop::vkutil::destroy(&reader);
    }

    // ---- 释放：再次 render 触发 fire_releases，帧生命周期闭合 ----
    scene.render({}, &reason);
    if (released != expected_releases)
        return fail("release callback not fired for every submitted frame");
    ::close(target_fd);
    ::close(nv12_fd);
    vkUnmapMemory(producer.device, smem);
    vkFreeMemory(producer.device, smem, nullptr);
    vkDestroyBuffer(producer.device, stage, nullptr);
    vkDestroyCommandPool(producer.device, pool, nullptr);
    vkDestroyImage(producer.device, image, nullptr);
    vkFreeMemory(producer.device, memory, nullptr);
    kop::vkutil::destroy(&producer);
    scene.shutdown();
    std::fprintf(stdout, "vk nv12 scene: PASS\n");
    return 0;
}
