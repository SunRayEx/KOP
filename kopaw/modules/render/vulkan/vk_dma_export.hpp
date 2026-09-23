// P3-M3：Vulkan 图像导出 DMA-BUF。
//
// VulkanDmabufExporter 把 CPU RGBA 帧上传到带外部内存导出的 VkImage，
// 产出 KOPAW_MEMORY_VULKAN 帧：
//   - planes[0] 携带导出的 DMA-BUF fd、offset/stride/modifier（跨进程可导入）；
//   - acquire_fence 携带 sync-fence fd（上传完成信号，消费者按 explicit-sync 等待）；
//   - dma_buf_handle 是生产者本地 VkImage 句柄（仅同进程 ABI 消费有效）。
// 像素全程驻留 GPU/显存，跨进程零拷贝；fd 与图像资源都由帧的 release 回调
// 统一回收（close_external_fds + external_release）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kop/vk_util.hpp"
#include "kopaw_abi.h"
#include "../../frame.hpp"

namespace kopaw {

namespace vkutil = kop::vkutil;

class VulkanDmabufExporter {
public:
    VulkanDmabufExporter() = default;
    ~VulkanDmabufExporter();
    VulkanDmabufExporter(const VulkanDmabufExporter&) = delete;
    VulkanDmabufExporter& operator=(const VulkanDmabufExporter&) = delete;

    // max_images 限制同时存在的导出图像数（空闲池 + 在飞帧）。
    bool init(uint32_t max_images, std::string* error);
    void shutdown();
    bool inited() const { return ctx_.device != VK_NULL_HANDLE; }

    // src 必须是 KOPAW_MEMORY_CPU 的 RGBA8 视频帧。返回的帧引用归零时：
    // 关闭 DMA-BUF/fence fd → 图像归还空闲池。池耗尽时返回 nullptr（调用方
    // 上游通过有界队列形成背压）。
    KopawFrame* export_cpu_frame(const KopawFrame* src, std::string* error);

    // 能力探测：当前加载器/驱动是否支持 DMA-BUF 导出（测试据此跳过）。
    static bool export_supported(std::string* reason);

    uint64_t exported_frames() const { return exported_frames_; }
    uint64_t upload_us_total() const { return upload_us_total_; }
    const std::string& device_name() const { return ctx_.device_name; }

    // 导出侧物理设备 UUID：导入侧据此核对是否同卡。
    const std::array<uint8_t, VK_UUID_SIZE>& device_uuid() const {
        return ctx_.device_uuid;
    }

private:
    struct Slot {
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t offset = 0;
        uint32_t stride = 0;
        uint64_t modifier = 0;
        bool in_use = false;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    bool ensure_slot(uint32_t w, uint32_t h, Slot** out, std::string* error);
    bool create_export_image(uint32_t w, uint32_t h, Slot* slot, std::string* error);
    bool ensure_upload_resources(size_t bytes, std::string* error);
    bool upload_and_export(const KopawFrame* src, Slot* slot, KopawFrame* out,
                           std::string* error);
    static void recycle_cb(void* ctx, OwnedFrame* frame);
    void recycle(Slot* slot);

    vkutil::DeviceContext ctx_{};
    uint32_t max_images_ = 0;
    uint32_t live_images_ = 0;
    // 稳定指针池：Slot 一经创建地址不变，在飞帧凭指针归还。
    std::vector<std::unique_ptr<Slot>> pool_;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    void* staging_map_ = nullptr;
    VkDeviceSize staging_size_ = 0;
    VkFence export_fence_ = VK_NULL_HANDLE;

    uint64_t exported_frames_ = 0;
    uint64_t upload_us_total_ = 0;
};

}  // namespace kopaw
