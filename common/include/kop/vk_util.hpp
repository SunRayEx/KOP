// 跨进程零拷贝共用的最小 Vulkan 装置（P3-M3）。
//
// KOPAW 导出器与 KOPMS 导入/合成器共享这份窗口无关的 instance/device 装配
// 代码；窗口 surface、交换链和管线由各自模块自持。约定：不缓存函数指针、
// 不创建任何 GUI 资源，错误经 *error 返回。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace kop {
namespace vkutil {

inline std::string to_hex(const std::array<uint8_t, VK_UUID_SIZE>& uuid) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(uuid.size() * 2);
    for (uint8_t b : uuid) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xf]);
    }
    return out;
}

struct DeviceContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    // 扩展探测结果（调用方据此选择 modifier/fence 路径）
    bool ext_drm_modifier = false;
    bool ext_dma_buf = false;
    bool ext_fence_fd = false;
    std::string device_name;
    // VK_KHR_external_memory_capabilities 设备 UUID：跨 VkDevice 导入 DMA-BUF
    // 时用来核对导出侧与导入侧是否同一张物理卡。
    std::array<uint8_t, VK_UUID_SIZE> device_uuid{};

    std::string uuid_hex() const { return to_hex(device_uuid); }
    // 加载器不静态导出的设备级扩展命令（经 vkGetDeviceProcAddr 解析）。
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
    PFN_vkGetFenceFdKHR get_fence_fd = nullptr;
};

// 创建实例；extra_instance_extensions 由调用方追加（如窗口 surface 扩展）。
bool create_instance(const std::vector<const char*>& extra_instance_extensions,
                     VkInstance* out, std::string* error);

// 选择支持图形队列与全部必需设备扩展的物理设备并创建 device。
// extra_extensions 由调用方追加（如 swapchain）。
// prefer_name 非空时，同名设备只要满足全部要求即优先选中：DMA-BUF 导入侧
// 必须与导出侧同卡（生产链路里合成器与客户端始终同机同卡），这能让双卡
// 机器上的导入测试确定性地落在同一张卡上。
bool pick_and_create_device(const std::vector<const char*>& extra_extensions,
                            DeviceContext* ctx, std::string* error,
                            const std::string& prefer_name = {});

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
                          VkMemoryPropertyFlags props, std::string* error);

inline void destroy(DeviceContext* ctx) {
    if (!ctx) return;
    if (ctx->device != VK_NULL_HANDLE) vkDestroyDevice(ctx->device, nullptr);
    if (ctx->instance != VK_NULL_HANDLE) vkDestroyInstance(ctx->instance, nullptr);
    ctx->device = VK_NULL_HANDLE;
    ctx->instance = VK_NULL_HANDLE;
    ctx->physical = VK_NULL_HANDLE;
}

}  // namespace vkutil
}  // namespace kop
