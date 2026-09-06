#include "vk_scene.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <utility>

#include "kop/log.h"
#include "kop/vk_util.hpp"

#ifdef KOPMS_SCENE_WINDOW
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

// 与 KOPAW 共用同一组全屏四边形 SPIR-V（kopms 构建期嵌入，符号相同）。
extern "C" {
extern const uint8_t kopaw_vert_spv[];
extern const unsigned int kopaw_vert_spv_len;
extern const uint8_t kopaw_frag_spv[];
extern const unsigned int kopaw_frag_spv_len;
}

namespace kopms {

namespace vkutil = kop::vkutil;

static const char* kTag = "kopms-scene";

namespace {

constexpr uint64_t kDrmFormatModInvalid = 0x00ffffffffffffffull;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool vkfail(VkResult r, const char* what, std::string* error) {
    if (r == VK_SUCCESS) return false;
    if (error) *error = std::string(what) + " failed (VkResult " +
                        std::to_string(static_cast<int>(r)) + ")";
    KOP_LOG_ERROR(kTag, "%s", error ? error->c_str() : what);
    return true;
}

// DRM fourcc → VkFormat（M3 覆盖单平面 RGBA；多平面 YUV 留待后续）。
bool drm_format_to_vk(uint32_t fourcc, VkFormat* out) {
    switch (fourcc) {
        case 0x34325258u:  // XR24 XRGB8888
        case 0x34325241u:  // AR24 ARGB8888
            *out = VK_FORMAT_B8G8R8A8_UNORM;
            return true;
        case 0x34324258u:  // XB24 XBGR8888
        case 0x34324241u:  // AB24 ABGR8888（KOPAW 导出格式）
            *out = VK_FORMAT_R8G8B8A8_UNORM;
            return true;
        default:
            return false;
    }
}

int dup_cloexec(int fd) {
    if (fd < 0) return -1;
    return static_cast<int>(fcntl(fd, F_DUPFD_CLOEXEC, 3));
}

}  // namespace

struct VulkanScene::Impl {
    VulkanScene* owner = nullptr;

    // 设备与命令
    vkutil::DeviceContext ctx{};
    Options options{};
    VkQueue queue = VK_NULL_HANDLE;

    // 管线
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    // 离屏/直出目标：双缓冲交替，均可导出 DMA-BUF
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        uint32_t stride = 0;
        uint64_t modifier = 0;
    };
    Target targets[2]{};
    uint32_t cur_target = 0;

    // 窗口呈现（可选）
    bool windowed = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swap_extent{0, 0};
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    std::vector<VkFramebuffer> swap_fbs;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    VkSemaphore present_sem = VK_NULL_HANDLE;
    uint32_t swap_index = 0;

    // 合成完成 fence（可导出 sync fd，供 explicit-sync release fence）
    VkFence composite_fence = VK_NULL_HANDLE;
    // 有未完成合成时为 true（导出 payload / 等待完成后复位）
    bool fence_in_flight_ = false;
    uint64_t composite_count_ = 0;
    // 最近一次已完成合成的 release fence fd（拥有；take_release_fence dup）
    int last_release_fd = -1;

    // 场景窗口的待呈现帧（每窗口最多一帧 = 回压边界）
    struct Item {
        uint64_t window_id = 0;
        uint64_t session_id = 0;
        uint32_t frame_id = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        bool layout_done = false;  // UNDEFINED→GENERAL 已转换
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet dset = VK_NULL_HANDLE;
    };
    std::map<uint64_t, Item> pending;
    std::vector<Item> presented;  // 已合成、待 GPU 完成/flip 后释放

    Stats stats{};

    void destroy_item_resources(Item* item) {
        if (item->dset != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(ctx.device, dpool, 1, &item->dset);
            item->dset = VK_NULL_HANDLE;
        }
        if (item->view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx.device, item->view, nullptr);
            item->view = VK_NULL_HANDLE;
        }
        if (item->image != VK_NULL_HANDLE) {
            vkDestroyImage(ctx.device, item->image, nullptr);
            item->image = VK_NULL_HANDLE;
        }
        if (item->memory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx.device, item->memory, nullptr);
            item->memory = VK_NULL_HANDLE;
        }
    }

    void fire_releases() {
        for (Item& item : presented) {
            if (getenv("KOPMS_RELEASE_DEBUG")) {
                KOP_LOG_DEBUG("kopms-scene", "fire release frame=%u win=%llu",
                              item.frame_id,
                              static_cast<unsigned long long>(item.window_id));
            }
            if (owner && owner->release_fn_) {
                owner->release_fn_(item.window_id, item.session_id, item.frame_id);
            }
            destroy_item_resources(&item);
        }
        presented.clear();
    }

    bool wait_composite_fence(std::string* error) {
        if (composite_fence == VK_NULL_HANDLE || !fence_in_flight_) return true;
        VkResult r = vkWaitForFences(ctx.device, 1, &composite_fence, VK_TRUE,
                                     static_cast<uint64_t>(options.fence_timeout_ms) *
                                         1000000ull);
        if (r == VK_TIMEOUT) {
            if (error) *error = "composite fence timeout";
            KOP_LOG_WARN(kTag, "合成 fence 等待超时（%d ms）", options.fence_timeout_ms);
            return false;
        }
        if (r != VK_SUCCESS) {
            vkfail(r, "vkWaitForFences(composite)", error);
            return false;
        }
        fence_in_flight_ = false;
        return true;
    }

    // 已 signal 的合成 fence 转移为 sync fd（fence 复位，下轮 submit 前会
    // 再 vkResetFences，语义不受影响）。
    void cache_release_fence() {
        if (last_release_fd >= 0) {
            ::close(last_release_fd);
            last_release_fd = -1;
        }
        if (composite_fence == VK_NULL_HANDLE || composite_count_ == 0) return;
        // 初始"创建即 signal"的 payload 不代表任何合成，拒绝导出的驱动
        // 会得到 -1；因此只在真实合成完成过后导出。
        VkFenceGetFdInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
        info.fence = composite_fence;
        info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd = -1;
        if (ctx.get_fence_fd && ctx.get_fence_fd(ctx.device, &info, &fd) == VK_SUCCESS &&
            fd >= 0) {
            last_release_fd = fd;
            fence_in_flight_ = false;  // payload 已转移，fence 复位
        }
    }

    bool create_render_pass(VkFormat format, std::string* error) {
        VkAttachmentDescription color{};
        color.format = format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // 便于 DMA-BUF 再导出/复用
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 1;
        rp.pAttachments = &color;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        return vkfail(vkCreateRenderPass(ctx.device, &rp, nullptr, &render_pass),
                      "vkCreateRenderPass", error);
    }

    bool create_pipeline(std::string* error) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkfail(vkCreateSampler(ctx.device, &si, nullptr, &sampler),
                   "vkCreateSampler", error)) {
            return false;
        }
        VkDescriptorSetLayoutBinding bind{};
        bind.binding = 0;
        bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = 1;
        dli.pBindings = &bind;
        if (vkfail(vkCreateDescriptorSetLayout(ctx.device, &dli, nullptr, &dsl),
                   "vkCreateDescriptorSetLayout", error)) {
            return false;
        }
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsl;
        if (vkfail(vkCreatePipelineLayout(ctx.device, &pli, nullptr,
                                          &pipeline_layout),
                   "vkCreatePipelineLayout", error)) {
            return false;
        }

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = kopaw_vert_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t*>(kopaw_vert_spv);
        VkShaderModule vs = VK_NULL_HANDLE;
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vkfail(vkCreateShaderModule(ctx.device, &smci, nullptr, &vs),
                   "vkCreateShaderModule(vs)", error)) {
            return false;
        }
        smci.codeSize = kopaw_frag_spv_len;
        smci.pCode = reinterpret_cast<const uint32_t*>(kopaw_frag_spv);
        if (vkfail(vkCreateShaderModule(ctx.device, &smci, nullptr, &fs),
                   "vkCreateShaderModule(fs)", error)) {
            vkDestroyShaderModule(ctx.device, vs, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;

        VkGraphicsPipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount = 2;
        pi.pStages = stages;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dyn;
        pi.layout = pipeline_layout;
        pi.renderPass = render_pass;
        const bool failed = vkfail(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE,
                                                             1, &pi, nullptr,
                                                             &pipeline),
                                   "vkCreateGraphicsPipelines", error);
        vkDestroyShaderModule(ctx.device, vs, nullptr);
        vkDestroyShaderModule(ctx.device, fs, nullptr);
        return failed;  // vkfail 惯例：true = 失败
    }

    bool create_target(Target& t, std::string* error) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent = {options.width, options.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExternalMemoryImageCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageDrmFormatModifierListCreateInfoEXT modifier_list{};
        const uint64_t linear = 0;  // DRM_FORMAT_MOD_LINEAR
        if (ctx.ext_drm_modifier) {
            modifier_list.sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
            modifier_list.drmFormatModifierCount = 1;
            modifier_list.pDrmFormatModifiers = &linear;
            external.pNext = &modifier_list;
            ii.pNext = &external;
        } else {
            ii.pNext = &external;
        }
        if (vkfail(vkCreateImage(ctx.device, &ii, nullptr, &t.image),
                   "vkCreateImage(target)", error)) {
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(ctx.device, t.image, &req);
        VkExportMemoryAllocateInfo export_mem{};
        export_mem.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_mem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = t.image;
        export_mem.pNext = &dedicated;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.pNext = &export_mem;
        ai.memoryTypeIndex = vkutil::find_memory_type(
            ctx.physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            error);
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (vkfail(vkAllocateMemory(ctx.device, &ai, nullptr, &t.memory),
                   "vkAllocateMemory(target)", error)) {
            return false;
        }
        if (vkfail(vkBindImageMemory(ctx.device, t.image, t.memory, 0),
                   "vkBindImageMemory(target)", error)) {
            return false;
        }
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout_res{};
        vkGetImageSubresourceLayout(ctx.device, t.image, &sub, &layout_res);
        t.stride = static_cast<uint32_t>(layout_res.rowPitch);
        t.modifier = ctx.ext_drm_modifier ? 0 : kDrmFormatModInvalid;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = t.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkfail(vkCreateImageView(ctx.device, &vi, nullptr, &t.view),
                   "vkCreateImageView(target)", error)) {
            return false;
        }
        VkFramebufferCreateInfo fbi{};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = render_pass;
        fbi.attachmentCount = 1;
        fbi.pAttachments = &t.view;
        fbi.width = options.width;
        fbi.height = options.height;
        fbi.layers = 1;
        return vkfail(vkCreateFramebuffer(ctx.device, &fbi, nullptr, &t.fb),
                      "vkCreateFramebuffer(target)", error);
    }

#ifdef KOPMS_SCENE_WINDOW
    bool create_swapchain(std::string* error) {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkfail(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical, surface,
                                                             &caps),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", error)) {
            return true;
        }
        uint32_t n_fmt = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical, surface, &n_fmt, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n_fmt);
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical, surface, &n_fmt,
                                             formats.data());
        swap_format = formats[0].format;
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                swap_format = f.format;
                break;
            }
        }
        swap_extent = caps.currentExtent;
        if (swap_extent.width == UINT32_MAX) {
            swap_extent = {options.width, options.height};
        }
        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
            image_count = caps.maxImageCount;
        }
        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = surface;
        sci.minImageCount = image_count;
        sci.imageFormat = swap_format;
        sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent = swap_extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        if (vkfail(vkCreateSwapchainKHR(ctx.device, &sci, nullptr, &swapchain),
                   "vkCreateSwapchainKHR", error)) {
            return true;
        }
        uint32_t n = 0;
        vkGetSwapchainImagesKHR(ctx.device, swapchain, &n, nullptr);
        swap_images.resize(n);
        vkGetSwapchainImagesKHR(ctx.device, swapchain, &n, swap_images.data());
        swap_views.resize(n);
        swap_fbs.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = swap_images[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swap_format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkfail(vkCreateImageView(ctx.device, &vi, nullptr, &swap_views[i]),
                       "vkCreateImageView(swap)", error)) {
                return true;
            }
            VkFramebufferCreateInfo fbi{};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = render_pass;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &swap_views[i];
            fbi.width = swap_extent.width;
            fbi.height = swap_extent.height;
            fbi.layers = 1;
            if (vkfail(vkCreateFramebuffer(ctx.device, &fbi, nullptr, &swap_fbs[i]),
                       "vkCreateFramebuffer(swap)", error)) {
                return true;
            }
        }
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkfail(vkCreateSemaphore(ctx.device, &sem, nullptr, &acquire_sem),
                   "vkCreateSemaphore(acquire)", error) ||
            vkfail(vkCreateSemaphore(ctx.device, &sem, nullptr, &present_sem),
                   "vkCreateSemaphore(present)", error)) {
            return true;
        }
        return false;
    }

    void destroy_swapchain() {
        for (auto fb : swap_fbs) vkDestroyFramebuffer(ctx.device, fb, nullptr);
        for (auto v : swap_views) vkDestroyImageView(ctx.device, v, nullptr);
        swap_fbs.clear();
        swap_views.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(ctx.device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        swap_images.clear();
        if (acquire_sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx.device, acquire_sem, nullptr);
            acquire_sem = VK_NULL_HANDLE;
        }
        if (present_sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(ctx.device, present_sem, nullptr);
            present_sem = VK_NULL_HANDLE;
        }
    }
#endif
};

VulkanScene::VulkanScene() = default;

VulkanScene::~VulkanScene() { shutdown(); }

bool VulkanScene::init(const Options& options, GLFWwindow* window, std::string* error) {
    if (inited_) return true;
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl& s = *impl_;
    s.owner = this;
    s.options = options;
    s.windowed = options.windowed;

    std::vector<const char*> instance_exts;
    std::vector<const char*> device_exts;
#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        if (!window) {
            if (error) *error = "windowed 场景需要 GLFW 窗口";
            return true;
        }
        uint32_t n = 0;
        const char** exts = glfwGetRequiredInstanceExtensions(&n);
        if (!exts) {
            if (error) *error = "GLFW 不支持 Vulkan surface";
            return true;
        }
        instance_exts.assign(exts, exts + n);
        device_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
#else
    (void)window;
    if (s.windowed) {
        if (error) *error = "场景窗口呈现未编译（KOPMS_SCENE_WINDOW=0）";
        return true;
    }
#endif

    if (!vkutil::create_instance(instance_exts, &s.ctx.instance, error)) return true;
    if (!vkutil::pick_and_create_device(device_exts, &s.ctx, error)) return true;
    s.queue = s.ctx.graphics_queue;

#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        const VkResult surface_result =
            glfwCreateWindowSurface(s.ctx.instance, window, nullptr, &s.surface);
        if (vkfail(surface_result, "glfwCreateWindowSurface", error)) return true;
        VkBool32 present_ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s.ctx.physical, s.ctx.graphics_family,
                                             s.surface, &present_ok);
        if (!present_ok) {
            if (error) *error = "图形队列不支持该窗口 surface 的呈现";
            return true;
        }
    }
#endif

    if (s.create_render_pass(VK_FORMAT_B8G8R8A8_UNORM, error)) return true;
    if (s.create_pipeline(error)) return true;

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = s.ctx.graphics_family;
    if (vkfail(vkCreateCommandPool(s.ctx.device, &pool, nullptr, &s.cmd_pool),
               "vkCreateCommandPool(scene)", error)) {
        return true;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = s.cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkfail(vkAllocateCommandBuffers(s.ctx.device, &alloc, &s.cmd),
               "vkAllocateCommandBuffers(scene)", error)) {
        return true;
    }

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 64;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkfail(vkCreateDescriptorPool(s.ctx.device, &dpi, nullptr, &s.dpool),
               "vkCreateDescriptorPool(scene)", error)) {
        return true;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 首轮 render 无需等待
    VkExportFenceCreateInfo efi{};
    efi.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
    efi.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    fci.pNext = &efi;
    if (vkfail(vkCreateFence(s.ctx.device, &fci, nullptr, &s.composite_fence),
               "vkCreateFence(composite)", error)) {
        return true;
    }

#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        if (s.create_swapchain(error)) return true;
        if (s.swap_format != VK_FORMAT_B8G8R8A8_UNORM) {
            // 交换链格式与初始 render pass 不兼容：重建 render pass + 管线
            vkDestroyPipeline(s.ctx.device, s.pipeline, nullptr);
            vkDestroyRenderPass(s.ctx.device, s.render_pass, nullptr);
            s.pipeline = VK_NULL_HANDLE;
            s.render_pass = VK_NULL_HANDLE;
            if (!s.create_render_pass(s.swap_format, error)) return true;
            if (s.create_pipeline(error)) return true;
        }
    } else
#endif
    {
        if (s.create_target(s.targets[0], error)) return true;
        if (s.create_target(s.targets[1], error)) return true;
    }

    inited_ = true;
    KOP_LOG_INFO(kTag, "Vulkan 场景就绪（%s %ux%u device=%s）",
                 s.windowed ? "windowed" : "offscreen", options.width, options.height,
                 s.ctx.device_name.c_str());
    return false;
}

void VulkanScene::shutdown() {
    if (!impl_) {
        inited_ = false;
        return;
    }
    Impl& s = *impl_;
    if (s.ctx.device != VK_NULL_HANDLE) vkDeviceWaitIdle(s.ctx.device);
    for (auto& item : s.pending) s.destroy_item_resources(&item.second);
    s.pending.clear();
    s.fire_releases();
#ifdef KOPMS_SCENE_WINDOW
    s.destroy_swapchain();
    if (s.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(s.ctx.instance, s.surface, nullptr);
        s.surface = VK_NULL_HANDLE;
    }
#endif
    for (Impl::Target& t : s.targets) {
        if (t.fb) vkDestroyFramebuffer(s.ctx.device, t.fb, nullptr);
        if (t.view) vkDestroyImageView(s.ctx.device, t.view, nullptr);
        if (t.image) vkDestroyImage(s.ctx.device, t.image, nullptr);
        if (t.memory) vkFreeMemory(s.ctx.device, t.memory, nullptr);
        t = {};
    }
    if (s.last_release_fd >= 0) ::close(s.last_release_fd);
    s.last_release_fd = -1;
    if (s.composite_fence) vkDestroyFence(s.ctx.device, s.composite_fence, nullptr);
    if (s.dpool) vkDestroyDescriptorPool(s.ctx.device, s.dpool, nullptr);
    if (s.pipeline) vkDestroyPipeline(s.ctx.device, s.pipeline, nullptr);
    if (s.pipeline_layout) {
        vkDestroyPipelineLayout(s.ctx.device, s.pipeline_layout, nullptr);
    }
    if (s.dsl) vkDestroyDescriptorSetLayout(s.ctx.device, s.dsl, nullptr);
    if (s.sampler) vkDestroySampler(s.ctx.device, s.sampler, nullptr);
    if (s.render_pass) vkDestroyRenderPass(s.ctx.device, s.render_pass, nullptr);
    if (s.cmd_pool) vkDestroyCommandPool(s.ctx.device, s.cmd_pool, nullptr);
    s.composite_fence = VK_NULL_HANDLE;
    s.dpool = VK_NULL_HANDLE;
    s.pipeline = VK_NULL_HANDLE;
    s.pipeline_layout = VK_NULL_HANDLE;
    s.dsl = VK_NULL_HANDLE;
    s.sampler = VK_NULL_HANDLE;
    s.render_pass = VK_NULL_HANDLE;
    s.cmd_pool = VK_NULL_HANDLE;
    vkutil::destroy(&s.ctx);
    inited_ = false;
}

bool VulkanScene::submit(uint64_t window_id, const ImportRequest& request,
                         std::string* error) {
    if (!inited_ || !impl_) {
        if (error) *error = "scene 未初始化";
        return false;
    }
    Impl& s = *impl_;
    if (s.pending.find(window_id) != s.pending.end()) {
        // 回压：该窗口上一帧仍在合成/呈现管线中。
        if (error) *error = "窗口上一帧仍在飞（回压拒收）";
        ++s.stats.rejected_frames;
        return false;
    }
    if (request.plane_count == 0 || request.plane_count > KOPAW_MAX_DMABUF_PLANES ||
        !request.plane_fds || request.width == 0 || request.height == 0) {
        if (error) *error = "import request 无效";
        ++s.stats.rejected_frames;
        return false;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!drm_format_to_vk(request.format, &format)) {
        if (error) *error = "不支持的 DRM 像素格式";
        ++s.stats.rejected_frames;
        return false;
    }
    // Vulkan 单一 VkImage 只能对应一份外部内存：多平面帧必须共享同一 fd。
    for (uint32_t i = 1; i < request.plane_count; ++i) {
        if (request.plane_fds[i] != request.plane_fds[0]) {
            if (error) *error = "多平面帧的平面必须位于同一 DMA-BUF";
            ++s.stats.rejected_frames;
            return false;
        }
    }

    const int64_t fence_begin = now_us();
    // explicit-sync 消费点：acquire fence 有界等待（sync fd signaled = POLLIN）。
    if (request.acquire_fence_kind == KOPAW_SYNC_FENCE_FD) {
        if (request.acquire_fence_fd < 0) {
            if (error) *error = "acquire fence fd 无效";
            return false;
        }
        pollfd pfd{request.acquire_fence_fd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, s.options.fence_timeout_ms);
        s.stats.fence_wait_us_total += static_cast<uint64_t>(now_us() - fence_begin);
        if (r == 0) {
            if (error) *error = "acquire fence 等待超时";
            return false;
        }
        if (r < 0) {
            if (error) *error = "acquire fence poll 失败";
            return false;
        }
    }
    const int64_t import_begin = now_us();

    Impl::Item item{};
    item.window_id = window_id;
    item.session_id = request.session_id;
    item.frame_id = request.frame_id;
    item.w = request.width;
    item.h = request.height;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {request.width, request.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod{};
    VkSubresourceLayout plane_layouts[KOPAW_MAX_DMABUF_PLANES] = {};
    if (s.ctx.ext_drm_modifier && request.plane_modifiers &&
        request.plane_modifiers[0] != kDrmFormatModInvalid) {
        for (uint32_t i = 0; i < request.plane_count; ++i) {
            plane_layouts[i].offset =
                request.plane_offsets ? request.plane_offsets[i] : 0;
            plane_layouts[i].rowPitch =
                request.plane_strides ? request.plane_strides[i] : 0;
        }
        explicit_mod.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        explicit_mod.drmFormatModifier = request.plane_modifiers[0];
        explicit_mod.drmFormatModifierPlaneCount = request.plane_count;
        explicit_mod.pPlaneLayouts = plane_layouts;
        external.pNext = &explicit_mod;
        ii.pNext = &external;
    } else {
        if (request.plane_modifiers && request.plane_modifiers[0] != 0 &&
            request.plane_modifiers[0] != kDrmFormatModInvalid) {
            if (error) *error = "未编译 DRM modifier 导入扩展，无法导入该 modifier";
            ++s.stats.rejected_frames;
            return false;
        }
        ii.pNext = &external;
    }
    if (vkfail(vkCreateImage(s.ctx.device, &ii, nullptr, &item.image),
               "vkCreateImage(import)", error)) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(s.ctx.device, item.image, &req);
    VkImportMemoryFdInfoKHR import_mem{};
    import_mem.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_mem.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_mem.fd = request.plane_fds[0];
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = item.image;
    import_mem.pNext = &dedicated;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.pNext = &import_mem;
    ai.memoryTypeIndex =
        vkutil::find_memory_type(s.ctx.physical, req.memoryTypeBits, 0, error);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        s.destroy_item_resources(&item);
        return false;
    }
    if (vkfail(vkAllocateMemory(s.ctx.device, &ai, nullptr, &item.memory),
               "vkAllocateMemory(import)", error)) {
        s.destroy_item_resources(&item);
        return false;
    }
    if (vkfail(vkBindImageMemory(s.ctx.device, item.image, item.memory, 0),
               "vkBindImageMemory(import)", error)) {
        s.destroy_item_resources(&item);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = item.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkfail(vkCreateImageView(s.ctx.device, &vi, nullptr, &item.view),
               "vkCreateImageView(import)", error)) {
        s.destroy_item_resources(&item);
        return false;
    }
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = s.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.dsl;
    if (vkfail(vkAllocateDescriptorSets(s.ctx.device, &dai, &item.dset),
               "vkAllocateDescriptorSets(import)", error)) {
        s.destroy_item_resources(&item);
        return false;
    }
    VkDescriptorImageInfo dii{};
    dii.sampler = s.sampler;
    dii.imageView = item.view;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = item.dset;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &dii;
    vkUpdateDescriptorSets(s.ctx.device, 1, &write, 0, nullptr);

    s.pending.emplace(window_id, item);
    s.stats.import_us_total += static_cast<uint64_t>(now_us() - import_begin);
    ++s.stats.imported_frames;
    return true;
}

bool VulkanScene::render(const std::vector<LayoutItem>& layout_items,
                         std::string* error) {
    if (!inited_ || !impl_) {
        if (error) *error = "scene 未初始化";
        return false;
    }
    Impl& s = *impl_;
    if (!direct_mode_) {
        // 呈现节拍：上一帧 GPU 完成 → 先导出本次合成 payload（作为
        // explicit-sync 的 release fence）→ 再释放上一组窗口帧。
        if (!s.wait_composite_fence(error)) return false;
        s.cache_release_fence();
        s.fire_releases();
    }
    if (getenv("KOPMS_RELEASE_DEBUG") && !s.pending.empty()) {
        std::string ids;
        for (const auto& p : s.pending) {
            ids += std::to_string(p.second.frame_id) + " ";
        }
        KOP_LOG_DEBUG("kopms-scene", "render: pending frames = %s", ids.c_str());
    }
    if (s.pending.empty()) return true;  // 无新内容也无待释放帧

    const int64_t started = now_us();
#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        VkResult acq = vkAcquireNextImageKHR(s.ctx.device, s.swapchain, UINT64_MAX,
                                             s.acquire_sem, VK_NULL_HANDLE,
                                             &s.swap_index);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s.ctx.device);
            s.destroy_swapchain();
            if (s.create_swapchain(error)) return false;
            acq = vkAcquireNextImageKHR(s.ctx.device, s.swapchain, UINT64_MAX,
                                        s.acquire_sem, VK_NULL_HANDLE, &s.swap_index);
        }
        if (vkfail(acq, "vkAcquireNextImageKHR", error)) return false;
    }
#endif

    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo cb_begin{};
    cb_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cb_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkfail(vkBeginCommandBuffer(s.cmd, &cb_begin),
               "vkBeginCommandBuffer(composite)", error)) {
        return false;
    }

    VkClearValue clear{};
    clear.color = {{0.03f, 0.03f, 0.045f, 1.0f}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = s.render_pass;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        rp.framebuffer = s.swap_fbs[s.swap_index];
        rp.renderArea.extent = s.swap_extent;
    } else
#endif
    {
        rp.framebuffer = s.targets[s.cur_target].fb;
        rp.renderArea.extent = {s.options.width, s.options.height};
    }
    vkCmdBeginRenderPass(s.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // z 升序绘制；调用方负责把焦点窗口排最后。
    std::vector<const LayoutItem*> order;
    order.reserve(layout_items.size());
    for (const auto& item : layout_items) {
        if (s.pending.find(item.window_id) != s.pending.end()) order.push_back(&item);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const LayoutItem* a, const LayoutItem* b) {
                         return a->z < b->z;
                     });
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
    std::vector<Impl::Item> drawn;
    if (getenv("KOPMS_RELEASE_DEBUG") && !order.empty()) {
        // 布局外挂起项检测：window 在 pending 但不在 layout → 永不绘制
        for (const auto& pend : s.pending) {
            bool in_layout = false;
            for (const auto* li : order) {
                if (li->window_id == pend.first) in_layout = true;
            }
            if (!in_layout) {
                KOP_LOG_WARN("kopms-scene",
                             "pending 项不在布局中：window=%llu（将滞留）",
                             static_cast<unsigned long long>(pend.first));
            }
        }
    }
    for (const LayoutItem* li : order) {
        auto it = s.pending.find(li->window_id);
        Impl::Item& item = it->second;
        if (!item.layout_done) {
            // 导入图像首个使用点：UNDEFINED→GENERAL（跨进程内容由 fence 保证）。
            VkImageMemoryBarrier to_general{};
            to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image = item.image;
            to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &to_general);
            item.layout_done = true;
        }
        const uint32_t fb_w = s.windowed ? s.swap_extent.width : s.options.width;
        const uint32_t fb_h = s.windowed ? s.swap_extent.height : s.options.height;
        if (li->rect.w == 0 || li->rect.h == 0 || li->rect.x >= (int32_t)fb_w ||
            li->rect.y >= (int32_t)fb_h) {
            continue;
        }
        const uint32_t w = std::min<uint32_t>(li->rect.w, fb_w - li->rect.x);
        const uint32_t h = std::min<uint32_t>(li->rect.h, fb_h - li->rect.y);
        VkViewport viewport{static_cast<float>(li->rect.x),
                            static_cast<float>(li->rect.y),
                            static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D scissor{{li->rect.x, li->rect.y}, {w, h}};
        vkCmdSetViewport(s.cmd, 0, 1, &viewport);
        vkCmdSetScissor(s.cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s.pipeline_layout, 0, 1, &item.dset, 0, nullptr);
        vkCmdDraw(s.cmd, 4, 1, 0, 0);
        drawn.push_back(item);
    }
    vkCmdEndRenderPass(s.cmd);
    if (vkfail(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer(composite)", error))
        return false;

    vkResetFences(s.ctx.device, 1, &s.composite_fence);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s.cmd;
#ifdef KOPMS_SCENE_WINDOW
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (s.windowed) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &s.acquire_sem;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &s.present_sem;
    }
#endif
    if (vkfail(vkQueueSubmit(s.queue, 1, &submit, s.composite_fence),
               "vkQueueSubmit(composite)", error)) {
        return false;
    }
    s.fence_in_flight_ = true;
    ++s.composite_count_;
#ifdef KOPMS_SCENE_WINDOW
    if (s.windowed) {
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &s.present_sem;
        present.swapchainCount = 1;
        present.pSwapchains = &s.swapchain;
        present.pImageIndices = &s.swap_index;
        const VkResult pr = vkQueuePresentKHR(s.queue, &present);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR &&
            pr != VK_ERROR_OUT_OF_DATE_KHR) {
            vkfail(pr, "vkQueuePresentKHR", error);
            return false;
        }
    }
#endif
    s.cur_target ^= 1u;
    for (Impl::Item& item : drawn) {
        s.pending.erase(item.window_id);
        s.presented.push_back(item);
    }
    s.stats.composite_us_total += static_cast<uint64_t>(now_us() - started);
    s.stats.presented_frames += drawn.size();
    return true;
}

int VulkanScene::take_release_fence() {
    if (!impl_) return -1;
    // render() 在等待上一合成完成后缓存 signaled fence fd；这里只做 dup。
    return dup_cloexec(impl_->last_release_fd);
}

VulkanScene::Stats VulkanScene::stats() const {
    return impl_ ? impl_->stats : Stats{};
}

bool VulkanScene::export_composite_dmabuf(int* fd, uint32_t* stride, uint32_t* width,
                                          uint32_t* height, std::string* error) {
    if (!inited_ || !impl_) {
        if (error) *error = "scene 未初始化";
        return false;
    }
    Impl& s = *impl_;
    // 直出路径：导出前必须等合成 fence，避免 KMS 扫描出撕裂的半成品。
    if (!s.wait_composite_fence(error)) return false;
    const Impl::Target& t = s.targets[s.cur_target ^ 1u];
    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = t.memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int out_fd = -1;
    if (vkfail(s.ctx.get_memory_fd(s.ctx.device, &info, &out_fd),
               "vkGetMemoryFdKHR(composite)", error)) {
        return false;
    }
    if (fd) *fd = out_fd;
    if (stride) *stride = t.stride;
    if (width) *width = s.options.width;
    if (height) *height = s.options.height;
    return true;
}

void VulkanScene::complete_direct_present() {
    if (!impl_) return;
    // page-flip 完成：上一组合成帧可以释放。
    impl_->fire_releases();
}

}  // namespace kopms
