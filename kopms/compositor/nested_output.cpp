#include "nested_output.hpp"

#include <cstring>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "kop/log.h"

namespace kopms {

static const char* kTag = "kopms-out";

namespace {
const char* kVert = R"(#version 330 core
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_uv;
out vec2 uv;
void main() { uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }
)";
const char* kFrag = R"(#version 330 core
in vec2 uv;
uniform sampler2D tex;
out vec4 o;
void main() { o = vec4(texture(tex, uv).rgb, 1.0); }
)";

void glfw_error(int code, const char* message) {
    KOP_LOG_ERROR(kTag, "GLFW error %d: %s", code, message ? message : "unknown");
}

GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        KOP_LOG_ERROR(kTag, "着色器编译失败: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}
}  // namespace

bool NestedOutput::init(int width, int height, const std::string& title) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        KOP_LOG_ERROR(kTag, "glfwInit 失败");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    win_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!win_) {
        KOP_LOG_ERROR(kTag, "窗口创建失败");
        return false;
    }
    glfwMakeContextCurrent(win_);
    glfwSwapInterval(1);

    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        KOP_LOG_ERROR(kTag, "程序链接失败: %s", log);
        return false;
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "tex"), 0);

    static const float quad[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,  //
        1.0f, -1.0f,  1.0f, 1.0f,  //
        -1.0f, 1.0f,  0.0f, 0.0f,  //
        1.0f, 1.0f,   1.0f, 0.0f,
    };
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindVertexArray(0);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);

    // 真实输入：GLFW 回调转交合成器输入管线（单窗口，静态路由安全）。
    glfwSetWindowUserPointer(win_, this);
    glfwSetCursorPosCallback(win_, [](GLFWwindow* window, double x, double y) {
        auto* self = static_cast<NestedOutput*>(glfwGetWindowUserPointer(window));
        if (!self || !self->callbacks_.cursor) return;
        // 窗口坐标 → framebuffer 坐标（HiDPI 缩放下两者不同）
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(window, &ww, &wh);
        glfwGetFramebufferSize(window, &fw, &fh);
        if (ww > 0 && wh > 0 && (fw != ww || fh != wh)) {
            x = x * fw / ww;
            y = y * fh / wh;
        }
        self->callbacks_.cursor(x, y);
    });
    glfwSetMouseButtonCallback(win_, [](GLFWwindow* window, int button, int action,
                                        int mods) {
        (void)mods;
        auto* self = static_cast<NestedOutput*>(glfwGetWindowUserPointer(window));
        if (self && self->callbacks_.mouse_button) {
            self->callbacks_.mouse_button(button, action);
        }
    });
    glfwSetKeyCallback(win_, [](GLFWwindow* window, int key, int scancode, int action,
                                int mods) {
        auto* self = static_cast<NestedOutput*>(glfwGetWindowUserPointer(window));
        if (self && self->callbacks_.key) self->callbacks_.key(key, scancode, action, mods);
    });

    inited_ = true;
    KOP_LOG_INFO(kTag, "嵌套输出就绪（%s）", glGetString(GL_VERSION));
    return true;
}

void NestedOutput::set_input_callbacks(InputCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

NestedOutput::ContentRect NestedOutput::content_rect() const {
    ContentRect rect{};
    if (!inited_) return rect;
    int w = 0, h = 0;
    glfwGetFramebufferSize(win_, &w, &h);
    rect.w = tex_w_;
    rect.h = tex_h_;
    rect.x = (w - tex_w_) / 2;
    rect.y = (h - tex_h_) / 2;
    return rect;
}

bool NestedOutput::ensure_texture(int32_t w, int32_t h, uint32_t fmt) {
    if (tex_w_ == w && tex_h_ == h && fmt_ == fmt && tex_) return false;
    fmt_ = fmt;
    tex_w_ = w;
    tex_h_ = h;
    glBindTexture(GL_TEXTURE_2D, tex_);
    // WL_SHM_FORMAT_ARGB8888/XRGB8888 内存布局即 GL_BGRA 小端直读
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    return true;
}

void NestedOutput::present(const void* data, int32_t w, int32_t h, int32_t stride,
                           uint32_t fmt) {
    if (!inited_ || w <= 0 || h <= 0) return;
    ensure_texture(w, h, fmt);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                    data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void NestedOutput::render_frame() {
    if (!inited_) return;
    int w = 0, h = 0;
    glfwGetFramebufferSize(win_, &w, &h);
    glViewport(0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex_w_ > 0 && tex_h_ > 0) {
        // 内容居中（MVP 不做缩放）
        int x = (w - tex_w_) / 2, y = (h - tex_h_) / 2;
        glViewport(x, y, tex_w_, tex_h_);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }
    glfwSwapBuffers(win_);
    glfwPollEvents();
}

bool NestedOutput::should_close() const {
    return win_ ? glfwWindowShouldClose(win_) != 0 : false;
}

void NestedOutput::shutdown() {
    if (!inited_) return;
    if (tex_) glDeleteTextures(1, &tex_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    tex_ = 0;
    vao_ = 0;
    program_ = 0;
    if (win_) {
        glfwDestroyWindow(win_);
        win_ = nullptr;
    }
    glfwTerminate();
    inited_ = false;
}

}  // namespace kopms
