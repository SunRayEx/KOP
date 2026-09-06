#version 450
// 直接采样 RGBA8 视频纹理。
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(texture(tex, uv).rgb, 1.0);
}
