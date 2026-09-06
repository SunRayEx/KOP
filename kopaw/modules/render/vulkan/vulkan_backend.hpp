// Vulkan 渲染后端（MVP 首选实现）。
// 管线：RGBA 纹理（双缓冲乒乓）→ 全屏三角形采样 → 动态渲染呈现。
// 使用 Vulkan 1.3 dynamic rendering（无 RenderPass 对象），FIFO 垂直同步。
// NV12/P010 DMA-BUF 按显式 DRM modifier 导入为双平面 VkImage，
// VkSamplerYcbcrConversion 完成有限范围 BT.601/709 转换。
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../render_backend.hpp"
#include "vk_ycbcr.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace kopaw {

class VulkanBackend final : public IRenderBackend {
public:
    ~VulkanBackend() override;

    bool init(GLFWwindow* window, std::string* error) override;
    bool draw(const KopawFrame* frame) override;
    void poll_events() override;
    bool window_closed() const override;
    void shutdown() override;
    const char* name() const override { return "vulkan"; }
    bool supports_dmabuf() const override {
        return inited_ && ycbcr_enabled_ && ext_dmabuf_ && ext_drm_modifier_ &&
               ext_foreign_queue_;
    }

private:
    static constexpr int kMaxFrames = 2;

    bool create_instance(std::string* error);
    bool create_surface(GLFWwindow* window, std::string* error);
    bool pick_physical_device(std::string* error);
    bool create_device(std::string* error);
    bool create_swapchain_objects(std::string* error);
    void destroy_swapchain_objects();
    bool create_pipeline(std::string* error);
    bool build_pipeline(VkPipelineLayout layout, VkPipeline* pipeline,
                        std::string* error);
    bool create_descriptors(std::string* error);
    bool create_sync_and_commands(std::string* error);
    bool ensure_texture(uint32_t w, uint32_t h, std::string* error);
    bool ensure_staging(int slot, VkDeviceSize size, std::string* error);
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props,
                              std::string* error) const;
    bool recreate_swapchain(std::string* error);

    // P2：DMABUF 两平面 YUV 导入。每个槽位持有最近一次导入的外部图像；
    // 复用槽位（inflight fence 已等待）时销毁旧导入。
    struct Imported {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        KopawFrame* frame = nullptr;
    };
    struct YuvPipeline {
        YcbcrConfig config;
        VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[kMaxFrames] = {};
    };
    YuvPipeline* ensure_yuv_pipeline(YcbcrConfig config, uint32_t width,
                                     uint32_t height, std::string* error);
    void destroy_yuv_pipeline(YuvPipeline& pipeline);
    bool import_yuv(int slot, const KopawFrame* frame, YuvPipeline& pipeline,
                    std::string* error);
    void destroy_imported(Imported& imp);

    struct Tex {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
    };

    GLFWwindow* win_ = nullptr;
    bool inited_ = false;

    VkInstance inst_ = VK_NULL_HANDLE;
    VkSurfaceKHR surf_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t gq_family_ = 0, pq_family_ = 0;
    VkQueue gq_ = VK_NULL_HANDLE, pq_ = VK_NULL_HANDLE;
    bool same_queue_ = true;

    bool ext_drm_modifier_ = false;
    bool ext_dmabuf_ = false;
    bool ext_foreign_queue_ = false;
    bool ycbcr_enabled_ = false;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;

    VkSwapchainKHR sc_ = VK_NULL_HANDLE;
    VkFormat sc_fmt_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D ext_{};
    std::vector<VkImage> sc_imgs_;
    std::vector<VkImageView> sc_views_;

    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout play_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dsets_[kMaxFrames]{};
    VkSampler samp_ = VK_NULL_HANDLE;

    // Format/modifier/matrix select immutable samplers and compatible layouts.
    // Fixed-function YCbCr conversion lets RGBA and YUV share quad.frag.
    std::vector<std::unique_ptr<YuvPipeline>> yuv_pipelines_;
    Imported imported_[kMaxFrames];

    Tex tex_[kMaxFrames];
    int cur_tex_ = 0;
    VkBuffer stage_[kMaxFrames]{};
    VkDeviceMemory stage_mem_[kMaxFrames]{};
    void* stage_map_[kMaxFrames]{};
    VkDeviceSize stage_size_[kMaxFrames] = {0, 0};

    VkCommandPool cpool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmds_[kMaxFrames]{};
    VkSemaphore img_avail_[kMaxFrames]{};
    VkSemaphore render_done_[kMaxFrames]{};
    VkFence inflight_[kMaxFrames]{};
    int frame_idx_ = 0;

    bool device_lost_ = false;
};

} // namespace kopaw
