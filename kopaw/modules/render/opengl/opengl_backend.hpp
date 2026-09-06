// OpenGL 渲染后端（P1 实现）：GL 3.3 core，双缓冲纹理 + letterbox + 垂直同步。
// 与 Vulkan 后端同一 IRenderBackend 抽象；GL 函数经 glvnd 直接链接
// （GL_GLEXT_PROTOTYPES，libGL.so 导出全套核心入口，无需 glad）。
#pragma once
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "../render_backend.hpp"

namespace kopaw {

class OpenglBackend final : public IRenderBackend {
public:
    ~OpenglBackend() override;

    bool init(GLFWwindow* window, std::string* error) override;
    bool draw(const KopawFrame* frame) override;
    void poll_events() override;
    bool window_closed() const override;
    void shutdown() override;
    const char* name() const override { return "opengl"; }

private:
    bool build_program(std::string* error);
    bool ensure_textures(uint32_t w, uint32_t h, std::string* error);
    void destroy_gl_objects();

    GLFWwindow* win_ = nullptr;
    bool inited_ = false;

    unsigned int program_ = 0;
    unsigned int vao_ = 0, vbo_ = 0;
    unsigned int tex_[2] = {0, 0};
    int cur_tex_ = 0;
    uint32_t tex_w_ = 0, tex_h_ = 0;
    int tex_loc_ = -1;
};

}  // namespace kopaw
