#include "kop/vk_util.hpp"

#include <cstring>

namespace kop {
namespace vkutil {

namespace {

bool check_vk(VkResult result, const char* what, std::string* error) {
    if (result == VK_SUCCESS) return true;
    if (error) {
        *error = std::string(what) + " failed (VkResult " +
                 std::to_string(static_cast<int>(result)) + ")";
    }
    return false;
}

}  // namespace

bool create_instance(const std::vector<const char*>& extra_instance_extensions,
                     VkInstance* out, std::string* error) {
    if (!out) return false;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "KOP";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "KOPAW/KOPMS";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extra_instance_extensions.size());
    ci.ppEnabledExtensionNames =
        extra_instance_extensions.empty() ? nullptr : extra_instance_extensions.data();
    return check_vk(vkCreateInstance(&ci, nullptr, out), "vkCreateInstance", error);
}

bool pick_and_create_device(const std::vector<const char*>& extra_extensions,
                            DeviceContext* ctx, std::string* error) {
    if (!ctx || ctx->instance == VK_NULL_HANDLE) {
        if (error) *error = "device selection requires an instance";
        return false;
    }
    uint32_t count = 0;
    if (!check_vk(vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr),
                  "vkEnumeratePhysicalDevices", error)) {
        return false;
    }
    if (count == 0) {
        if (error) *error = "没有可用的 Vulkan 物理设备";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!check_vk(vkEnumeratePhysicalDevices(ctx->instance, &count, devices.data()),
                  "vkEnumeratePhysicalDevices", error)) {
        return false;
    }

    // 离散显卡优先；要求图形队列 + 全部所需设备扩展。
    static const char* kBaseExtensions[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    };
    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t best_family = 0;
    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                 families.data());
        uint32_t graphics = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphics = i;
                break;
            }
        }
        if (graphics == UINT32_MAX) continue;

        uint32_t extension_count = 0;
        if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                 nullptr) != VK_SUCCESS) {
            continue;
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                 extensions.data()) != VK_SUCCESS) {
            continue;
        }
        auto has_extension = [&](const char* name) {
            for (const auto& ext : extensions) {
                if (std::strcmp(ext.extensionName, name) == 0) return true;
            }
            return false;
        };
        bool ok = true;
        for (const char* name : kBaseExtensions) ok = ok && has_extension(name);
        for (const char* name : extra_extensions) ok = ok && has_extension(name);
        if (!ok) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                              ? 500
                              : 100;
        if (score > best_score) {
            best_score = score;
            best = candidate;
            best_family = graphics;
            ctx->device_name = props.deviceName;
        }
    }
    if (best == VK_NULL_HANDLE) {
        if (error) *error = "没有支持图形队列和 DMA-BUF 外部内存扩展的物理设备";
        return false;
    }

    ctx->physical = best;
    ctx->graphics_family = best_family;
    ctx->ext_dma_buf = true;  // 已通过扩展列表过滤
    for (const char* name : extra_extensions) {
        if (std::strcmp(name, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0) {
            ctx->ext_drm_modifier = true;
        } else if (std::strcmp(name, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME) == 0) {
            ctx->ext_fence_fd = true;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = best_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    std::vector<const char*> extensions;
    for (const char* name : kBaseExtensions) extensions.push_back(name);
    for (const char* name : extra_extensions) extensions.push_back(name);

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queue;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (!check_vk(vkCreateDevice(best, &ci, nullptr, &ctx->device), "vkCreateDevice",
                  error)) {
        return false;
    }
    vkGetDeviceQueue(ctx->device, best_family, 0, &ctx->graphics_queue);
    // 设备级外部内存/栅栏命令不经加载器导出，按需解析。
    ctx->get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdKHR"));
    ctx->get_fence_fd = reinterpret_cast<PFN_vkGetFenceFdKHR>(
        vkGetDeviceProcAddr(ctx->device, "vkGetFenceFdKHR"));
    if (!ctx->get_memory_fd || !ctx->get_fence_fd) {
        if (error) *error = "无法解析 vkGetMemoryFdKHR/vkGetFenceFdKHR";
        vkDestroyDevice(ctx->device, nullptr);
        ctx->device = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
                          VkMemoryPropertyFlags props, std::string* error) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (memory.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    if (error) *error = "找不到满足属性的 Vulkan 内存类型";
    return UINT32_MAX;
}

}  // namespace vkutil
}  // namespace kop
