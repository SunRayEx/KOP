#include "vk_dma_export.hpp"

#include <cstddef>
#include <chrono>
#include <cstring>
#include <utility>

#include "kop/log.h"
#include "../../frame.hpp"

namespace kopaw {

static const char* kTag = "vk-export";

namespace {

bool vkfail(VkResult r, const char* what, std::string* error) {
    if (r == VK_SUCCESS) return false;
    if (error) *error = std::string(what) + " failed (VkResult " +
                        std::to_string(static_cast<int>(r)) + ")";
    KOP_LOG_ERROR(kTag, "%s", error ? error->c_str() : what);
    return true;
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

bool VulkanDmabufExporter::export_supported(std::string* reason) {
    VkInstance instance = VK_NULL_HANDLE;
    if (!vkutil::create_instance({}, &instance, reason)) return false;
    vkutil::DeviceContext ctx{};
    ctx.instance = instance;
    const bool ok = vkutil::pick_and_create_device({}, &ctx, reason);
    vkutil::destroy(&ctx);  // 连同实例一起销毁
    return ok;
}

VulkanDmabufExporter::~VulkanDmabufExporter() { shutdown(); }

bool VulkanDmabufExporter::init(uint32_t max_images, std::string* error) {
    if (inited()) return true;
    if (max_images == 0) {
        if (error) *error = "exporter 需要 max_images > 0";
        return true;
    }
    if (!vkutil::create_instance({}, &ctx_.instance, error)) return true;
    if (!vkutil::pick_and_create_device({}, &ctx_, error)) return true;

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = ctx_.graphics_family;
    if (vkfail(vkCreateCommandPool(ctx_.device, &pool, nullptr, &cmd_pool_),
               "vkCreateCommandPool", error)) {
        return true;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmd_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkfail(vkAllocateCommandBuffers(ctx_.device, &alloc, &cmd_),
               "vkAllocateCommandBuffers", error)) {
        return true;
    }
    // sync-fence 可导出：每次 submit 后经 vkGetFenceFdKHR 取走 payload 并复位。
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkExportFenceCreateInfo export_fence{};
    export_fence.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    export_fence.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    fence.pNext = &export_fence;
    if (vkfail(vkCreateFence(ctx_.device, &fence, nullptr, &export_fence_),
               "vkCreateFence(exportable)", error)) {
        return true;
    }
    max_images_ = max_images;
    KOP_LOG_INFO(kTag, "DMA-BUF 导出器就绪（device=%s max_images=%u）",
                 ctx_.device_name.c_str(), max_images_);
    return false;
}

void VulkanDmabufExporter::shutdown() {
    if (ctx_.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(ctx_.device);
    // 在飞帧持有的图像不归 shutdown 管（节点销毁前引擎已回收全部帧）；
    // 这里只释放空闲池与上传资源。
    for (auto& slot : pool_) {
        if (!slot->in_use) {
            if (slot->image) vkDestroyImage(ctx_.device, slot->image, nullptr);
            if (slot->memory) vkFreeMemory(ctx_.device, slot->memory, nullptr);
            slot->image = VK_NULL_HANDLE;
            slot->memory = VK_NULL_HANDLE;
        }
    }
    pool_.clear();
    live_images_ = 0;
    if (staging_map_) vkUnmapMemory(ctx_.device, staging_mem_);
    if (staging_) vkDestroyBuffer(ctx_.device, staging_, nullptr);
    if (staging_mem_) vkFreeMemory(ctx_.device, staging_mem_, nullptr);
    if (export_fence_) vkDestroyFence(ctx_.device, export_fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(ctx_.device, cmd_pool_, nullptr);
    staging_ = VK_NULL_HANDLE;
    staging_mem_ = VK_NULL_HANDLE;
    staging_map_ = nullptr;
    staging_size_ = 0;
    export_fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    vkutil::destroy(&ctx_);
    live_images_ = 0;
    max_images_ = 0;
}

bool VulkanDmabufExporter::create_export_image(uint32_t w, uint32_t h, Slot* slot,
                                               std::string* error) {
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {w, h, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    // VkImageDrmFormatModifierListCreateInfoEXT 只能与
    // VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT 配对。此前把它错误地作为
    // OPTIMAL 图像创建，部分驱动虽接受导出，却会在另一个 VkDevice 导入时
    // 以 vkAllocateMemory 失败暴露出来。
    // modifier 扩展缺失时退回 LINEAR tiling（行距有定义、全平台可导入）。
    image.tiling = ctx_.ext_drm_modifier
                       ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                       : VK_IMAGE_TILING_LINEAR;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageDrmFormatModifierListCreateInfoEXT modifier_list{};
    const uint64_t linear = kDrmFormatModLinear;
    if (ctx_.ext_drm_modifier) {
        // 线性 modifier 全平台可导入（含 llvmpipe/vkms/双显卡 PRIME）。
        modifier_list.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        modifier_list.drmFormatModifierCount = 1;
        modifier_list.pDrmFormatModifiers = &linear;
        external.pNext = &modifier_list;
        image.pNext = &external;
    } else {
        image.pNext = &external;
    }
    if (vkfail(vkCreateImage(ctx_.device, &image, nullptr, &slot->image),
               "vkCreateImage(exportable)", error)) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx_.device, slot->image, &req);
    VkExportMemoryAllocateInfo export_mem{};
    export_mem.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_mem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = slot->image;
    export_mem.pNext = &dedicated;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.pNext = &export_mem;
    // External-memory implementations (notably software Vulkan and some
    // integrated GPUs) may expose exportable image allocations only from a
    // host-visible heap. The image's memoryTypeBits already constrain this to
    // valid heaps; do not impose DEVICE_LOCAL and turn a usable zero-copy
    // handle into an avoidable OUT_OF_DEVICE_MEMORY failure.
    ai.memoryTypeIndex = vkutil::find_memory_type(ctx_.physical, req.memoryTypeBits,
                                                  0, error);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkfail(vkAllocateMemory(ctx_.device, &ai, nullptr, &slot->memory),
               "vkAllocateMemory(exportable)", error)) {
        return false;
    }
    if (vkfail(vkBindImageMemory(ctx_.device, slot->image, slot->memory, 0),
               "vkBindImageMemory", error)) {
        return false;
    }

    // 查询驱动实际采用的行距/偏移。modifier 图像按规范必须用
    // MEMORY_PLANE aspect 查询（COLOR 对部分驱动返回 0）。
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    if (ctx_.ext_drm_modifier) {
        sub.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    }
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(ctx_.device, slot->image, &sub, &layout);
    if (layout.rowPitch == 0) {
        sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkGetImageSubresourceLayout(ctx_.device, slot->image, &sub, &layout);
    }
    slot->stride = static_cast<uint32_t>(layout.rowPitch);
    slot->offset = static_cast<uint32_t>(layout.offset);
    slot->w = w;
    slot->h = h;
    slot->modifier = linear;  // 两条路径最终都是 DRM_FORMAT_MOD_LINEAR
    KOP_LOG_DEBUG(kTag, "导出图像 %ux%u stride=%u offset=%u modifier=0x%llx", w, h,
                  slot->stride, slot->offset,
                  static_cast<unsigned long long>(slot->modifier));
    if (slot->stride < w * 4) {
        if (error) *error = "导出图像行距异常";
        return false;
    }
    return true;
}

bool VulkanDmabufExporter::ensure_slot(uint32_t w, uint32_t h, Slot** out,
                                       std::string* error) {
    for (auto& slot : pool_) {
        if (!slot->in_use && slot->w == w && slot->h == h) {
            slot->in_use = true;
            *out = slot.get();
            return true;
        }
    }
    if (live_images_ >= max_images_) {
        // 消费者尚未释放旧帧：不阻塞，让上游队列背压。
        if (error) *error = "导出图像池已耗尽（等待 FRAME_RELEASE）";
        return false;
    }
    auto slot = std::make_unique<Slot>();
    slot->in_use = true;
    if (!create_export_image(w, h, slot.get(), error)) return false;
    ++live_images_;
    *out = slot.get();
    pool_.push_back(std::move(slot));
    return true;
}

bool VulkanDmabufExporter::ensure_upload_resources(size_t bytes, std::string* error) {
    if (staging_ != VK_NULL_HANDLE && staging_size_ >= bytes) return true;
    if (staging_map_) vkUnmapMemory(ctx_.device, staging_mem_);
    if (staging_) vkDestroyBuffer(ctx_.device, staging_, nullptr);
    if (staging_mem_) vkFreeMemory(ctx_.device, staging_mem_, nullptr);
    staging_ = VK_NULL_HANDLE;
    staging_mem_ = VK_NULL_HANDLE;
    staging_map_ = nullptr;
    staging_size_ = 0;

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkfail(vkCreateBuffer(ctx_.device, &bi, nullptr, &staging_),
               "vkCreateBuffer(staging)", error)) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx_.device, staging_, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vkutil::find_memory_type(
        ctx_.physical, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        error);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkfail(vkAllocateMemory(ctx_.device, &ai, nullptr, &staging_mem_),
               "vkAllocateMemory(staging)", error)) {
        return false;
    }
    if (vkfail(vkBindBufferMemory(ctx_.device, staging_, staging_mem_, 0),
               "vkBindBufferMemory", error)) {
        return false;
    }
    if (vkfail(vkMapMemory(ctx_.device, staging_mem_, 0, req.size, 0, &staging_map_),
               "vkMapMemory(staging)", error)) {
        return false;
    }
    staging_size_ = req.size;
    return true;
}

bool VulkanDmabufExporter::upload_and_export(const KopawFrame* src, Slot* slot,
                                             KopawFrame* out, std::string* error) {
    const int64_t started = now_us();
    const uint32_t w = src->format.video.width;
    const uint32_t h = src->format.video.height;
    const uint8_t* pixels = cpu_data(src);
    if (!pixels) {
        if (error) *error = "export_cpu_frame 需要 CPU 帧输入";
        return false;
    }
    if (!ensure_upload_resources(static_cast<size_t>(w) * h * 4, error)) return false;
    std::memcpy(staging_map_, pixels, static_cast<size_t>(w) * h * 4);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd_, 0);
    if (vkfail(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer", error))
        return false;

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // 上一消费者已完成读取
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = slot->image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_dst);

    // 源行距 ≠ 导出行距时由 bufferRowLength 声明 texel 行宽。
    VkBufferImageCopy region{};
    region.bufferRowLength = src->stride ? src->stride / 4 : w;
    region.bufferImageHeight = h;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, staging_, slot->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_general = to_dst;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_general);
    if (vkfail(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer", error)) return false;

    vkResetFences(ctx_.device, 1, &export_fence_);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    if (vkfail(vkQueueSubmit(ctx_.graphics_queue, 1, &submit, export_fence_),
               "vkQueueSubmit(upload)", error)) {
        return false;
    }
    // 导出 sync-fence fd（payload 转移给 fd，fence 复位）。
    VkFenceGetFdInfoKHR get_fd{};
    get_fd.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
    get_fd.fence = export_fence_;
    get_fd.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    int fence_fd = -1;
    if (vkfail(ctx_.get_fence_fd(ctx_.device, &get_fd, &fence_fd),
               "vkGetFenceFdKHR", error)) {
        return false;
    }

    VkMemoryGetFdInfoKHR mem_fd{};
    mem_fd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    mem_fd.memory = slot->memory;
    mem_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int dma_fd = -1;
    if (vkfail(ctx_.get_memory_fd(ctx_.device, &mem_fd, &dma_fd),
               "vkGetMemoryFdKHR", error)) {
        ::close(fence_fd);
        return false;
    }

    out->memory_type = KOPAW_MEMORY_VULKAN;
    out->format.video.width = w;
    out->format.video.height = h;
    out->size = static_cast<size_t>(w) * h * 4;  // 打包尺寸（stride 另行描述）
    out->stride = slot->stride;
    out->dma_fd = dma_fd;
    out->plane_count = 1;
    out->planes[0].fd = dma_fd;
    out->planes[0].offset = slot->offset;
    out->planes[0].stride = slot->stride;
    out->planes[0].modifier = slot->modifier;
    out->acquire_fence.kind = KOPAW_SYNC_FENCE_FD;
    out->acquire_fence.fd = fence_fd;
    out->acquire_fence.value = 0;
    out->dma_buf_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slot->image));
    out->flags = src->flags;
    const size_t color_end = offsetof(KopawFrame, color) + sizeof(src->color);
    if (src->struct_size >= color_end) out->color = src->color;
    upload_us_total_ += static_cast<uint64_t>(now_us() - started);
    ++exported_frames_;
    return true;
}

KopawFrame* VulkanDmabufExporter::export_cpu_frame(const KopawFrame* src,
                                                   std::string* error) {
    if (!inited() || !src) {
        if (error) *error = "exporter 未初始化或输入为空";
        return nullptr;
    }
    if (src->media_type != KOPAW_MEDIA_VIDEO || src->memory_type != KOPAW_MEMORY_CPU ||
        src->format.video.width == 0 || src->format.video.height == 0) {
        if (error) *error = "export_cpu_frame 需要 RGBA 视频帧";
        return nullptr;
    }
    Slot* slot = nullptr;
    if (!ensure_slot(src->format.video.width, src->format.video.height, &slot, error))
        return nullptr;

    OwnedFrame* out = make_external_frame(src->media_type, src->pts, src->dts);
    if (!upload_and_export(src, slot, &out->frame, error)) {
        recycle(slot);
        out->release_cb(&out->frame);
        return nullptr;
    }
    out->frame.user_data = slot;
    out->external_ctx = this;
    out->external_release = &VulkanDmabufExporter::recycle_cb;
    return out->ptr();
}

void VulkanDmabufExporter::recycle_cb(void* ctx, OwnedFrame* frame) {
    // close_external_fds 已关闭帧携带的 DMA-BUF/fence fd；这里归还图像。
    auto* exporter = static_cast<VulkanDmabufExporter*>(ctx);
    auto* slot = static_cast<Slot*>(frame->frame.user_data);
    delete frame;
    if (exporter && slot) exporter->recycle(slot);
}

void VulkanDmabufExporter::recycle(Slot* slot) {
    if (!slot) return;
    slot->in_use = false;
}

}  // namespace kopaw
