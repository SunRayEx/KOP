#version 450
// 两平面 YUV 采样 + YUV→RGB（P2 硬解零拷贝导入路径）。
// 采样器挂 VkSamplerYcbcrConversion（RGB_IDENTITY 模型）：硬件完成平面提取
// 与色度上采样，但不做 YCbCr→RGB 转换，采样返回的是格式通道的原序。
// G8_B8R8 / G10X6_B10X6R10X6 两平面格式中 R、B 取自 chroma 平面，G 取自
// luma 平面，故 texture().rgb == (Cr, Y, Cb)。range/matrix/transfer 是帧
// ABI 中的显式枚举，经 push constant 传入；分辨率从不参与色彩推断。
#extension GL_GOOGLE_include_directive : enable
#include "color.glsl"

layout(binding = 0) uniform sampler2D tex_yuv;
layout(push_constant) uniform Push {
    int bits10;
    int matrix;
    int range;
    int transfer;    // KOPAW_COLOR_TRANSFER_*；UNKNOWN = 直通
    float peak_in;   // 内容声明峰值 cd/m²（PQ/HLG 色调映射锚点）
    int out_mode;    // 0 = SDR sRGB；1 = HDR10 PQ
    int primaries;   // KOPAW_COLOR_PRIMARIES_*
    float sdr_ref;   // SDR 参考白 cd/m²（<=0 用 BT.2408 的 203）
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 s = texture(tex_yuv, uv).rgb;  // (Cr, Y, Cb)

    // 通道顺序与格式通道一致：luma 在 G，chroma 的 Cb 在 B、Cr 在 R。
    float y_raw = s.g;
    float u_raw = s.b;
    float v_raw = s.r;

    float y, u, v;
    if (pc.range == 2) {  // KOPAW_COLOR_RANGE_FULL
        y = y_raw;
        u = u_raw - 0.5;
        v = v_raw - 0.5;
    } else {
        // Limited range: 8bit black/white = 16/235, chroma = 128+-112;
        // P010 uses the corresponding 64/940 and 512+-448 representation.
        float q, yb, ys, cz, ch;
        if (pc.bits10 != 0) {
            q = 1023.0; yb = 64.0; ys = 876.0; cz = 512.0; ch = 448.0;
        } else {
            q = 255.0; yb = 16.0; ys = 219.0; cz = 128.0; ch = 112.0;
        }
        y = (y_raw * q - yb) / ys;
        u = (u_raw * q - cz) / (2.0 * ch);
        v = (v_raw * q - cz) / (2.0 * ch);
    }

    vec3 rgb;
    if (pc.matrix == 3) {  // KOPAW_COLOR_MATRIX_BT2020_NCL
        rgb = vec3(y + 1.475 * v, y - 0.165 * u - 0.571 * v, y + 1.881 * u);
    } else if (pc.matrix == 2) {  // KOPAW_COLOR_MATRIX_BT709
        rgb = vec3(y + 1.793 * v, y - 0.213 * u - 0.533 * v, y + 2.112 * u);
    } else {
        rgb = vec3(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u);
    }
    out_color = vec4(clamp(kopaw_apply_color_out(rgb, pc.transfer, pc.peak_in,
                                              pc.out_mode, pc.primaries,
                                              pc.sdr_ref),
                           0.0, 1.0), 1.0);
}
