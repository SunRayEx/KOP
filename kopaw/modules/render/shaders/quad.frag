#version 450
// 直接采样 RGBA8 视频纹理，并按帧 ABI 声明的 transfer 做色彩管线处理
// （EOTF → HDR 色调映射 → sRGB OETF）。transfer 未知时整条管线直通。
#extension GL_GOOGLE_include_directive : enable
#include "color.glsl"

layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Push {
    int bits10;      // 本路径未用（nv12 专用），保持布局一致
    int matrix;      // 本路径未用
    int range;       // 本路径未用
    int transfer;    // KOPAW_COLOR_TRANSFER_*
    float peak_in;   // 内容声明峰值 cd/m²（PQ/HLG 有效）
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 rgb = texture(tex, uv).rgb;
    out_color = vec4(clamp(kopaw_apply_color(rgb, pc.transfer, pc.peak_in),
                           0.0, 1.0), 1.0);
}
