#version 450
// 两平面 YUV 采样 + YUV→RGB（P2 硬解零拷贝导入路径）。
// 采样器挂 VkSamplerYcbcrConversion（RGB_IDENTITY 模型）：硬件完成平面提取
// 与色度上采样，采样返回 rgb = (Y, Cb, Cr)；位深与色域矩阵由 push constant
// 选择：bits10（1 = P010 / 0 = NV12）、bt709（1 = BT.709 / 0 = BT.601），
// 与 CPU 路径 sws 默认行为一致（有限范围量化）。
layout(binding = 0) uniform sampler2D tex_yuv;
layout(push_constant) uniform Push {
    int bits10;
    int bt709;
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 s = texture(tex_yuv, uv).rgb;  // (Y, Cb, Cr)

    // ITU-R BT.601/BT.709 有限范围量化：8bit 黑 16/白 235/色度 128±112；
    // 10bit 黑 64/白 940/色度 512±448
    float q, yb, ys, cz, ch;
    if (pc.bits10 != 0) {
        q = 1023.0; yb = 64.0; ys = 876.0; cz = 512.0; ch = 448.0;
    } else {
        q = 255.0; yb = 16.0; ys = 219.0; cz = 128.0; ch = 112.0;
    }
    float y = (s.x * q - yb) / ys;              // [0,1]
    float u = (s.y * q - cz) / (2.0 * ch);      // [-0.5,0.5]
    float v = (s.z * q - cz) / (2.0 * ch);

    vec3 rgb;
    if (pc.bt709 != 0) {
        rgb = vec3(y + 1.793 * v, y - 0.213 * u - 0.533 * v, y + 2.112 * u);
    } else {
        rgb = vec3(y + 1.596 * v, y - 0.391 * u - 0.813 * v, y + 2.018 * u);
    }
    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
