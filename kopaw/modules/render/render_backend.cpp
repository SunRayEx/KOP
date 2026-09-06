#include "render_backend.hpp"

#include "kop/log.h"

#if KOPAW_BUILD_VULKAN
#include "vulkan/vulkan_backend.hpp"
#endif

#if KOPAW_BUILD_OPENGL
#include "opengl/opengl_backend.hpp"
#endif

namespace kopaw {

static const char* kTag = "render";

IRenderBackend* create_render_backend(const std::string& name, std::string* error) {
#if KOPAW_BUILD_VULKAN
    if (name == "vulkan") {
        return new VulkanBackend();
    }
#else
    if (name == "vulkan") {
        *error = "本构建未编译 Vulkan 后端（KOPAW_BUILD_VULKAN=OFF）";
        return nullptr;
    }
#endif
    if (name == "opengl") {
#if KOPAW_BUILD_OPENGL
        return new OpenglBackend();
#else
        *error = "本构建未编译 OpenGL 后端（KOPAW_BUILD_OPENGL=OFF）";
        return nullptr;
#endif
    }
    *error = "未知渲染后端: " + name + "（可用: vulkan" +
#if KOPAW_BUILD_OPENGL
             "/opengl"
#endif
             "）";
    KOP_LOG_ERROR(kTag, "%s", error->c_str());
    return nullptr;
}

}  // namespace kopaw
