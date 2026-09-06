#include "vulkan_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "kop/log.h"
#include "../../frame.hpp"

// 由 CMake 嵌入步骤生成（shaders/*.vert|frag → SPIR-V 字节数组）
extern "C" {
extern const uint8_t kopaw_vert_spv[];
extern const unsigned int kopaw_vert_spv_len;
extern const uint8_t kopaw_frag_spv[];
extern const unsigned int kopaw_frag_spv_len;
}

namespace kopaw {

static const char* kTag = "vk";

namespace {

const char* vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "VK_ERROR";
    }
}

bool vkfail(VkResult r, const char* what, std::string* error) {
    if (r == VK_SUCCESS) return false;
    *error = std::string(what) + " 失败: " + vk_result_str(r);
    KOP_LOG_ERROR(kTag, "%s", error->c_str());
    return true;
}

}  // namespace

VulkanBackend::~VulkanBackend() { shutdown(); }

bool VulkanBackend::init(GLFWwindow* window, std::string* error) {
    win_ = window;
    if (create_instance(error) || create_surface(window, error) ||
        pick_physical_device(error) || create_device(error) ||
        create_swapchain_objects(error) || create_pipeline(error) ||
        create_descriptors(error) || create_sync_and_commands(error)) {
        return false;
    }
    inited_ = true;
    KOP_LOG_INFO(kTag, "Vulkan 后端就绪 (%ux%u)", ext_.width, ext_.height);
    return true;
}

void VulkanBackend::poll_events() { glfwPollEvents(); }

bool VulkanBackend::window_closed() const { return glfwWindowShouldClose(win_); }

bool VulkanBackend::create_instance(std::string* error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "kopaw-player";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "KOPAW";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    uint32_t n_ext = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&n_ext);
    if (!glfw_exts) {
        *error = "GLFW 未获得 Vulkan 窗口扩展支持";
        return true;
    }
    std::vector<const char*> exts(glfw_exts, glfw_exts + n_ext);

    std::vector<const char*> layers;
    const char* want_validation = getenv("KOPAW_VK_VALIDATION");
    if (want_validation && want_validation[0] == '1') {
        uint32_t n_layer = 0;
        vkEnumerateInstanceLayerProperties(&n_layer, nullptr);
        std::vector<VkLayerProperties> avail(n_layer);
        vkEnumerateInstanceLayerProperties(&n_layer, avail.data());
        for (auto& l : avail) {
            if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                KOP_LOG_INFO(kTag, "启用 Vulkan 校验层");
                break;
            }
        }
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    return vkfail(vkCreateInstance(&ci, nullptr, &inst_), "vkCreateInstance", error);
}

bool VulkanBackend::create_surface(GLFWwindow* window, std::string* error) {
    return vkfail(glfwCreateWindowSurface(inst_, window, nullptr, &surf_),
                  "glfwCreateWindowSurface", error);
}

bool VulkanBackend::pick_physical_device(std::string* error) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    if (n == 0) {
        *error = "没有可用的 Vulkan 物理设备";
        return true;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst_, &n, devs.data());

    // 离散显卡优先，独显/集显各打分
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        uint32_t n_qf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n_qf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(n_qf);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &n_qf, qfs.data());

        uint32_t gq = UINT32_MAX, pq = UINT32_MAX;
        for (uint32_t i = 0; i < n_qf; ++i) {
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gq = std::min(gq, i);
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surf_, &present);
            if (present) pq = std::min(pq, i);
        }
        if (gq == UINT32_MAX || pq == UINT32_MAX) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 1000
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500
                                                                                 : 100;
        if (score > best_score) {
            best_score = score;
            best = d;
            gq_family_ = gq;
            pq_family_ = pq;
        }
    }
    if (!best) {
        *error = "没有支持图形+呈现队列的物理设备";
        return true;
    }
    pd_ = best;
    same_queue_ = gq_family_ == pq_family_;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd_, &props);
    KOP_LOG_INFO(kTag, "设备: %s (%s)", props.deviceName,
                 props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
                                                                          : "integrated");
    return false;
}

bool VulkanBackend::create_device(std::string* error) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2]{};
    uint32_t n_q = same_queue_ ? 1 : 2;
    qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qcis[0].queueFamilyIndex = gq_family_;
    qcis[0].queueCount = 1;
    qcis[0].pQueuePriorities = &prio;
    if (!same_queue_) {
        qcis[1] = qcis[0];
        qcis[1].queueFamilyIndex = pq_family_;
    }

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = n_q;
    ci.pQueueCreateInfos = qcis;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = dev_exts;
    if (vkfail(vkCreateDevice(pd_, &ci, nullptr, &dev_), "vkCreateDevice", error)) return true;

    vkGetDeviceQueue(dev_, gq_family_, 0, &gq_);
    vkGetDeviceQueue(dev_, pq_family_, 0, &pq_);
    return false;
}

uint32_t VulkanBackend::find_memory_type(uint32_t type_bits,
                                         VkMemoryPropertyFlags props,
                                         std::string* error) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    *error = "找不到合适的显存类型";
    return UINT32_MAX;
}

bool VulkanBackend::create_swapchain_objects(std::string* error) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkfail(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd_, surf_, &caps),
               "获取表面能力", error))
        return true;

    uint32_t n_fmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surf_, &n_fmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n_fmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd_, surf_, &n_fmt, fmts.data());
    sc_fmt_ = fmts[0].format;
    for (auto& f : fmts) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sc_fmt_ = f.format;
            break;
        }
    }

    uint32_t n_pm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surf_, &n_pm, nullptr);
    std::vector<VkPresentModeKHR> pms(n_pm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd_, surf_, &n_pm, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;  // 保证存在
    for (auto m : pms) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) pm = m;  // 有则低延迟
    }

    ext_ = caps.currentExtent;
    if (ext_.width == UINT32_MAX) {
        int w, h;
        glfwGetFramebufferSize(win_, &w, &h);
        ext_.width = static_cast<uint32_t>(w);
        ext_.height = static_cast<uint32_t>(h);
    }
    if (ext_.width == 0 || ext_.height == 0) return false;  // 最小化中，稍后重建

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surf_;
    ci.minImageCount = img_count;
    ci.imageFormat = sc_fmt_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t families[2] = {gq_family_, pq_family_};
    if (same_queue_) {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = families;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;
    if (vkfail(vkCreateSwapchainKHR(dev_, &ci, nullptr, &sc_), "vkCreateSwapchainKHR", error))
        return true;

    uint32_t n_img = 0;
    vkGetSwapchainImagesKHR(dev_, sc_, &n_img, nullptr);
    sc_imgs_.resize(n_img);
    vkGetSwapchainImagesKHR(dev_, sc_, &n_img, sc_imgs_.data());

    sc_views_.resize(n_img);
    for (uint32_t i = 0; i < n_img; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = sc_imgs_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sc_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkfail(vkCreateImageView(dev_, &vi, nullptr, &sc_views_[i]),
                   "vkCreateImageView", error))
            return true;
    }
    return false;
}

void VulkanBackend::destroy_swapchain_objects() {
    for (auto v : sc_views_) vkDestroyImageView(dev_, v, nullptr);
    sc_views_.clear();
    if (sc_) {
        vkDestroySwapchainKHR(dev_, sc_, nullptr);
        sc_ = VK_NULL_HANDLE;
    }
    sc_imgs_.clear();
}

bool VulkanBackend::recreate_swapchain(std::string* error) {
    vkDeviceWaitIdle(dev_);
    destroy_swapchain_objects();
    if (create_swapchain_objects(error)) return true;
    // 视口随交换链变化：销毁旧管线并重建（布局/描述符布局保留）
    if (pipe_) {
        vkDestroyPipeline(dev_, pipe_, nullptr);
        pipe_ = VK_NULL_HANDLE;
    }
    if (create_pipeline(error)) return true;
    return false;
}

bool VulkanBackend::create_pipeline(std::string* error) {
    // --- 采样器（一次） ---
    if (!samp_) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkfail(vkCreateSampler(dev_, &si, nullptr, &samp_), "vkCreateSampler", error))
            return true;
    }

    // --- 着色器模块 ---
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = kopaw_vert_spv_len;
    smci.pCode = reinterpret_cast<const uint32_t*>(kopaw_vert_spv);
    VkShaderModule vs{};
    if (vkfail(vkCreateShaderModule(dev_, &smci, nullptr, &vs), "顶点着色器模块", error))
        return true;
    smci.codeSize = kopaw_frag_spv_len;
    smci.pCode = reinterpret_cast<const uint32_t*>(kopaw_frag_spv);
    VkShaderModule fs{};
    if (vkfail(vkCreateShaderModule(dev_, &smci, nullptr, &fs), "片段着色器模块", error)) {
        vkDestroyShaderModule(dev_, vs, nullptr);
        return true;
    }

    // --- 描述符布局（一次） ---
    if (!dsl_) {
        VkDescriptorSetLayoutBinding bind{};
        bind.binding = 0;
        bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = 1;
        dli.pBindings = &bind;
        if (vkfail(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &dsl_),
                   "描述符布局", error)) {
            return true;
        }
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsl_;
        if (vkfail(vkCreatePipelineLayout(dev_, &pli, nullptr, &play_), "管线布局", error)) {
            return true;
        }
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

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dyn;
    pi.layout = play_;
    if (vkfail(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe_),
               "图形管线", error)) {
        vkDestroyShaderModule(dev_, vs, nullptr);
        vkDestroyShaderModule(dev_, fs, nullptr);
        return true;
    }
    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    return false;
}

bool VulkanBackend::create_descriptors(std::string* error) {
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = kMaxFrames;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxFrames;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkfail(vkCreateDescriptorPool(dev_, &pi, nullptr, &dpool_), "描述符池", error))
        return true;

    for (int i = 0; i < kMaxFrames; ++i) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = dpool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsl_;
        if (vkfail(vkAllocateDescriptorSets(dev_, &ai, &dsets_[i]), "分配描述符集", error))
            return true;
    }
    return false;
}

bool VulkanBackend::create_sync_and_commands(std::string* error) {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = gq_family_;
    if (vkfail(vkCreateCommandPool(dev_, &cpi, nullptr, &cpool_), "命令池", error))
        return true;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cpool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kMaxFrames;
    if (vkfail(vkAllocateCommandBuffers(dev_, &cai, cmds_), "命令缓冲", error)) return true;

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kMaxFrames; ++i) {
        if (vkfail(vkCreateSemaphore(dev_, &sci, nullptr, &img_avail_[i]), "信号量", error) ||
            vkfail(vkCreateSemaphore(dev_, &sci, nullptr, &render_done_[i]), "信号量", error) ||
            vkfail(vkCreateFence(dev_, &fci, nullptr, &inflight_[i]), "栅栏", error)) {
            return true;
        }
    }
    return false;
}

bool VulkanBackend::ensure_staging(int slot, VkDeviceSize size, std::string* error) {
    if (stage_size_[slot] >= size) return false;
    if (stage_[slot]) {
        vkDestroyBuffer(dev_, stage_[slot], nullptr);
        vkFreeMemory(dev_, stage_mem_[slot], nullptr);
        stage_[slot] = VK_NULL_HANDLE;
        stage_mem_[slot] = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkfail(vkCreateBuffer(dev_, &bi, nullptr, &stage_[slot]), "上传缓冲", error))
        return true;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev_, stage_[slot], &req);
    uint32_t mt = find_memory_type(req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   error);
    if (mt == UINT32_MAX) return true;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    if (vkfail(vkAllocateMemory(dev_, &ai, nullptr, &stage_mem_[slot]), "分配上传内存", error))
        return true;
    if (vkfail(vkBindBufferMemory(dev_, stage_[slot], stage_mem_[slot], 0),
               "绑定上传缓冲", error))
        return true;
    if (vkfail(vkMapMemory(dev_, stage_mem_[slot], 0, req.size, 0, &stage_map_[slot]),
               "映射上传缓冲", error))
        return true;
    stage_size_[slot] = req.size;
    return false;
}

bool VulkanBackend::ensure_texture(uint32_t w, uint32_t h, std::string* error) {
    if (tex_[0].w == w && tex_[0].h == h && tex_[0].img) return false;

    // 尺寸变化：先销毁旧纹理
    for (int i = 0; i < kMaxFrames; ++i) {
        if (tex_[i].view) vkDestroyImageView(dev_, tex_[i].view, nullptr);
        if (tex_[i].img) vkDestroyImage(dev_, tex_[i].img, nullptr);
        if (tex_[i].mem) vkFreeMemory(dev_, tex_[i].mem, nullptr);
        tex_[i] = Tex{};
    }

    for (int i = 0; i < kMaxFrames; ++i) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {w, h, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkfail(vkCreateImage(dev_, &ii, nullptr, &tex_[i].img), "创建采样图像", error))
            return true;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev_, tex_[i].img, &req);
        uint32_t mt = find_memory_type(req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error);
        if (mt == UINT32_MAX) return true;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mt;
        if (vkfail(vkAllocateMemory(dev_, &ai, nullptr, &tex_[i].mem), "分配显存", error))
            return true;
        if (vkfail(vkBindImageMemory(dev_, tex_[i].img, tex_[i].mem, 0), "绑定显存", error))
            return true;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = tex_[i].img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkfail(vkCreateImageView(dev_, &vi, nullptr, &tex_[i].view), "采样图像视图", error))
            return true;
        tex_[i].w = w;
        tex_[i].h = h;

        // 描述符集指向对应纹理
        VkDescriptorImageInfo dii{};
        dii.sampler = samp_;
        dii.imageView = tex_[i].view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = dsets_[i];
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    }
    cur_tex_ = 0;
    return false;
}

bool VulkanBackend::draw(const KopawFrame* frame) {
    if (device_lost_ || !inited_) return false;
    std::string err;

    const uint32_t w = frame->format.video.width;
    const uint32_t h = frame->format.video.height;
    if (w == 0 || h == 0) return true;

    // 窗口缩放：帧缓冲尺寸与交换链不一致时重建（含最小化恢复）
    {
        int fb_w = 0, fb_h = 0;
        glfwGetFramebufferSize(win_, &fb_w, &fb_h);
        if ((fb_w > 0 && fb_h > 0 &&
             (static_cast<uint32_t>(fb_w) != ext_.width ||
              static_cast<uint32_t>(fb_h) != ext_.height)) ||
            ext_.width == 0) {
            if (recreate_swapchain(&err)) {
                KOP_LOG_ERROR(kTag, "%s", err.c_str());
                device_lost_ = true;
                return false;
            }
        }
    }
    if (ensure_texture(w, h, &err)) {
        KOP_LOG_ERROR(kTag, "%s", err.c_str());
        device_lost_ = true;
        return false;
    }
    if (ensure_staging(cur_tex_, static_cast<VkDeviceSize>(frame->size), &err)) {
        KOP_LOG_ERROR(kTag, "%s", err.c_str());
        device_lost_ = true;
        return false;
    }
    const uint8_t* input = cpu_data(frame);
    if (!input) {
        KOP_LOG_ERROR(kTag, "Vulkan 后端收到不可 CPU 映射的帧句柄");
        device_lost_ = true;
        return false;
    }
    memcpy(stage_map_[cur_tex_], input, frame->size);

    const int fi = frame_idx_;
    vkWaitForFences(dev_, 1, &inflight_[fi], VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult acq = vkAcquireNextImageKHR(dev_, sc_, UINT64_MAX, img_avail_[fi],
                                         VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        if (recreate_swapchain(&err)) {
            KOP_LOG_ERROR(kTag, "%s", err.c_str());
            device_lost_ = true;
            return false;
        }
        return true;  // 本帧跳过
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        KOP_LOG_ERROR(kTag, "acquire 失败: %s", vk_result_str(acq));
        device_lost_ = true;
        return false;
    }
    vkResetFences(dev_, 1, &inflight_[fi]);

    VkCommandBuffer cmd = cmds_[fi];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);

    // 1) staging → 纹理（含布局转换）
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = tex_[cur_tex_].img;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_dst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, stage_[cur_tex_], tex_[cur_tex_].img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_read);

    // 2) 交换链图像 → 颜色附件
    VkImageMemoryBarrier to_color{};
    to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image = sc_imgs_[img_idx];
    to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_color);

    VkClearValue clear = {{{0.02f, 0.02f, 0.03f, 1.0f}}};
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    att.imageView = sc_views_[img_idx];
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue = clear;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    ri.renderArea = {{0, 0}, ext_.width, ext_.height};  // 清屏覆盖整窗（含黑边）
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    // letterbox：保持视频纵横比居中，黑边由 clear 填充
    const LetterboxRect lb = compute_letterbox(w, h, ext_.width, ext_.height);
    VkViewport viewport{static_cast<float>(lb.x), static_cast<float>(lb.y),
                        static_cast<float>(lb.w), static_cast<float>(lb.h), 0.0f, 1.0f};
    VkRect2D scissor{{lb.x, lb.y},
                     {static_cast<uint32_t>(lb.w), static_cast<uint32_t>(lb.h)}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, play_, 0, 1,
                            &dsets_[cur_tex_], 0, nullptr);
    vkCmdDraw(cmd, 4, 1, 0, 0);  // 三角带全屏四边形
    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = sc_imgs_[img_idx];
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_present);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &img_avail_[fi];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[fi];
    if (vkfail(vkQueueSubmit(gq_, 1, &si, inflight_[fi]), "vkQueueSubmit", &err)) {
        device_lost_ = true;
        KOP_LOG_ERROR(kTag, "%s", err.c_str());
        return false;
    }

    VkPresentInfoKHR pri{};
    pri.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pri.waitSemaphoreCount = 1;
    pri.pWaitSemaphores = &render_done_[fi];
    pri.swapchainCount = 1;
    pri.pSwapchains = &sc_;
    pri.pImageIndices = &img_idx;
    VkResult pr = vkQueuePresentKHR(pq_, &pri);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        if (recreate_swapchain(&err)) {
            KOP_LOG_ERROR(kTag, "%s", err.c_str());
            device_lost_ = true;
            return false;
        }
    } else if (pr != VK_SUCCESS) {
        KOP_LOG_ERROR(kTag, "present 失败: %s", vk_result_str(pr));
        device_lost_ = true;
        return false;
    }

    frame_idx_ = (frame_idx_ + 1) % kMaxFrames;
    cur_tex_ = (cur_tex_ + 1) % kMaxFrames;
    return true;
}

void VulkanBackend::shutdown() {
    if (!inited_ && dev_ == VK_NULL_HANDLE) {
        // init 未完成时也要清理已创建的部分
    }
    if (dev_ != VK_NULL_HANDLE) vkDeviceWaitIdle(dev_);
    for (int i = 0; i < kMaxFrames; ++i) {
        if (stage_[i]) vkDestroyBuffer(dev_, stage_[i], nullptr);
        if (stage_mem_[i]) vkFreeMemory(dev_, stage_mem_[i], nullptr);
        stage_[i] = VK_NULL_HANDLE;
        stage_mem_[i] = VK_NULL_HANDLE;
        stage_map_[i] = nullptr;
        stage_size_[i] = 0;
        if (tex_[i].view) vkDestroyImageView(dev_, tex_[i].view, nullptr);
        if (tex_[i].img) vkDestroyImage(dev_, tex_[i].img, nullptr);
        if (tex_[i].mem) vkFreeMemory(dev_, tex_[i].mem, nullptr);
        tex_[i] = Tex{};
        if (img_avail_[i]) vkDestroySemaphore(dev_, img_avail_[i], nullptr);
        if (render_done_[i]) vkDestroySemaphore(dev_, render_done_[i], nullptr);
        if (inflight_[i]) vkDestroyFence(dev_, inflight_[i], nullptr);
        img_avail_[i] = render_done_[i] = VK_NULL_HANDLE;
        inflight_[i] = VK_NULL_HANDLE;
    }
    if (pipe_) vkDestroyPipeline(dev_, pipe_, nullptr);
    if (play_) vkDestroyPipelineLayout(dev_, play_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (samp_) vkDestroySampler(dev_, samp_, nullptr);
    if (cpool_) vkDestroyCommandPool(dev_, cpool_, nullptr);
    cpool_ = VK_NULL_HANDLE;
    destroy_swapchain_objects();
    if (surf_) vkDestroySurfaceKHR(inst_, surf_, nullptr);
    if (dev_) vkDestroyDevice(dev_, nullptr);
    if (inst_) vkDestroyInstance(inst_, nullptr);
    pipe_ = VK_NULL_HANDLE;
    play_ = VK_NULL_HANDLE;
    dsl_ = VK_NULL_HANDLE;
    dpool_ = VK_NULL_HANDLE;
    samp_ = VK_NULL_HANDLE;
    surf_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE;
    inst_ = VK_NULL_HANDLE;
    inited_ = false;
}

}  // namespace kopaw
