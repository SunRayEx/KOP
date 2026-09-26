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
    int out_mode;    // 0 = SDR sRGB；1 = HDR10 PQ
    int primaries;   // KOPAW_COLOR_PRIMARIES_*
    float sdr_ref;   // SDR 参考白 cd/m²（<=0 用 BT.2408 的 203）
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 rgb = texture(tex, uv).rgb;
    out_color = vec4(clamp(kopaw_apply_color_out(rgb, pc.transfer, pc.peak_in,
                                              pc.out_mode, pc.primaries,
                                              pc.sdr_ref),
                           0.0, 1.0), 1.0);
}
