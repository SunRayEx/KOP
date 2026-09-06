#include "opengl_backend.hpp"

#include <cstring>
#include <string>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "../../frame.hpp"
#include "kop/log.h"

namespace kopaw {

static const char* kTag = "gl";

namespace {

const char* kVertSrc = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 uv;
void main() {
    uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

const char* kFragSrc = R"(#version 330 core
in vec2 uv;
uniform sampler2D tex;
out vec4 out_color;
void main() {
    out_color = vec4(texture(tex, uv).rgb, 1.0);
}
)";

GLuint compile_shader(GLenum type, const char* src, std::string* error) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        *error = std::string("着色器编译失败: ") + log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

OpenglBackend::~OpenglBackend() { shutdown(); }

bool OpenglBackend::init(GLFWwindow* window, std::string* error) {
    win_ = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // 垂直同步

    // 约定：init 返回 true = 成功；build_program 返回 true = 失败
    if (build_program(error)) {
        return false;
    }

    // 全屏四边形（三角带）：pos.xy ∈ [-1,1]，uv 让图像正立
    static const float quad[] = {
        // pos        uv
        -1.0f, -1.0f, 0.0f, 1.0f,  //
        1.0f, -1.0f,  1.0f, 1.0f,  //
        -1.0f, 1.0f,  0.0f, 0.0f,  //
        1.0f, 1.0f,   1.0f, 0.0f,
    };
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    tex_loc_ = glGetUniformLocation(program_, "tex");
    glGenTextures(2, tex_);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindVertexArray(0);

    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    inited_ = true;
    KOP_LOG_INFO(kTag, "OpenGL 后端就绪（%s）", glGetString(GL_VERSION));
    return true;
}

bool OpenglBackend::build_program(std::string* error) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertSrc, error);
    if (!vs) return true;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragSrc, error);
    if (!fs) {
        glDeleteShader(vs);
        return true;
    }
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
        *error = std::string("程序链接失败: ") + log;
        return true;
    }
    glUseProgram(program_);
    return false;
}

bool OpenglBackend::ensure_textures(uint32_t w, uint32_t h, std::string*) {
    if (tex_w_ == w && tex_h_ == h && tex_[0]) return false;
    tex_w_ = w;
    tex_h_ = h;
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w),
                     static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    return false;
}

bool OpenglBackend::draw(const KopawFrame* frame) {
    if (!inited_) return false;
    const uint32_t w = frame->format.video.width;
    const uint32_t h = frame->format.video.height;
    if (w == 0 || h == 0) return true;
    const uint8_t* input = cpu_data(frame);
    if (!input) {
        KOP_LOG_ERROR(kTag, "OpenGL 后端收到不可 CPU 映射的帧句柄");
        return false;
    }

    ensure_textures(w, h, nullptr);

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(win_, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return true;

    // 上传当前帧（stride 经 UNPACK_ROW_LENGTH 支持非紧凑行）
    glBindTexture(GL_TEXTURE_2D, tex_[cur_tex_]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  frame->stride > 0 ? static_cast<GLint>(frame->stride / 4)
                                    : static_cast<GLint>(w));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w),
                    static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, input);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // letterbox：清整窗，视口只覆盖视频区域
    const LetterboxRect lb = compute_letterbox(w, h, static_cast<uint32_t>(fb_w),
                                               static_cast<uint32_t>(fb_h));
    glViewport(lb.x, lb.y, lb.w, lb.h);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);
    glUniform1i(tex_loc_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_[cur_tex_]);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glfwSwapBuffers(win_);
    cur_tex_ = (cur_tex_ + 1) % 2;
    return true;
}

void OpenglBackend::poll_events() { glfwPollEvents(); }

bool OpenglBackend::window_closed() const { return glfwWindowShouldClose(win_); }

void OpenglBackend::destroy_gl_objects() {
    if (tex_[0]) glDeleteTextures(2, tex_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    tex_[0] = tex_[1] = 0;
    vao_ = vbo_ = 0;
    program_ = 0;
    tex_w_ = tex_h_ = 0;
}

void OpenglBackend::shutdown() {
    if (!inited_) return;
    destroy_gl_objects();
    glfwMakeContextCurrent(nullptr);
    inited_ = false;
}

}  // namespace kopaw
