// 共享色彩管线：EOTF 线性化 + HDR→SDR 色调映射 + sRGB 编码。
// 被 quad.frag / nv12.frag 引用（glslang 的 GL_GOOGLE_include_directive）。
// 本文件不带 #version：由引入它的着色器决定 GLSL 版本与内置函数可用性。
//
// 契约（与 common/include/kop/color_pipeline.hpp 同源）：
//   - 合成器输出为 8-bit UNORM 缓冲，编码约定为 sRGB（IEC 61966-2-1）。
//     因此片段着色器产出的必须是经 sRGB OETF 编码后的非线性值。
//   - 内容侧按帧 ABI 声明的 transfer 线性化；transfer 未知时整条管线直通
//     （保留历史行为，不做任何色彩推断）。
//   - PQ 的 EOTF 输出是绝对亮度（cd/m²，上限 10000），HLG 的 EOTF 输出是
//     相对亮度 [0,1]。两者都以“内容声明峰值”为锚做色调映射归一化到 [0,1]，
//     其中参考白（峰值）映射到 1.0，高光按 2-1/t 软收敛。

// KOPAW_COLOR_TRANSFER_*（kopaw_abi.h）
#define KOPAW_TF_UNKNOWN 0
#define KOPAW_TF_BT709   1
#define KOPAW_TF_SRGB    2
#define KOPAW_TF_GAMMA22 3
#define KOPAW_TF_PQ      4
#define KOPAW_TF_HLG     5

// 单通道 EOTF：非线性编码值 → 线性亮度（PQ 为 cd/m²，其余为相对值）。
float kopaw_eotf1(float v, int tf) {
    if (tf == KOPAW_TF_SRGB) {
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    if (tf == KOPAW_TF_GAMMA22) {
        return pow(clamp(v, 0.0, 1.0), 2.2);
    }
    if (tf == KOPAW_TF_PQ) {
        // SMPTE ST 2084；输出 10000 * E 为绝对亮度 cd/m²。
        const float m1 = 2610.0 / 16384.0;
        const float m2 = 2523.0 / 4096.0 * 128.0;
        const float c1 = 3424.0 / 4096.0;
        const float c2 = 2413.0 / 4096.0 * 32.0;
        const float c3 = 2392.0 / 4096.0 * 32.0;
        float em = pow(clamp(v, 0.0, 1.0), 1.0 / m2);
        return 10000.0 * pow(max(em - c1, 0.0) / (c2 - c3 * em), 1.0 / m1);
    }
    if (tf == KOPAW_TF_HLG) {
        // ARIB STD-B67；输出相对亮度 [0,1]，参考峰值 1000 cd/m²。
        const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
        return v <= 0.5 ? v * v / 3.0 : (exp((v - c) / a) + b) / 12.0;
    }
    // BT.709（SMPTE 170M/240M 在 ABI 中归一到同一枚举）。
    return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
}

vec3 kopaw_eotf(vec3 v, int tf) {
    if (tf == KOPAW_TF_UNKNOWN) return v;  // 未声明：直通，不推断
    return vec3(kopaw_eotf1(v.r, tf), kopaw_eotf1(v.g, tf), kopaw_eotf1(v.b, tf));
}

// HDR 线性亮度 → SDR 相对亮度 [0,1]。
// t = luma/peak_in：t<=1 线性保持（参考白映射到 1.0），t>1 按 2-1/t 软收敛
// （渐近 2，保留高光细节而非硬切）。输出已除以 peak_in，是显示无关的相对值。
vec3 kopaw_tonemap(vec3 lin, float peak_in) {
    const float luma = dot(lin, vec3(0.2126, 0.7152, 0.0722));
    const float t = luma / max(peak_in, 1e-6);
    const float u = t <= 1.0 ? t : 2.0 - 1.0 / t;
    const float scale = (t <= 1.0 ? 1.0 : u / t) / max(peak_in, 1e-6);
    return lin * scale;
}

float kopaw_srgb_oetf1(float l) {
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(max(l, 0.0), 1.0 / 2.4) - 0.055;
}

vec3 kopaw_srgb_oetf(vec3 l) {
    return vec3(kopaw_srgb_oetf1(l.r), kopaw_srgb_oetf1(l.g),
                kopaw_srgb_oetf1(l.b));
}

// 完整链：非线性 RGB →（EOTF）→ 线性 →（HDR 色调映射）→（sRGB OETF）→ 输出。
vec3 kopaw_apply_color(vec3 nonlin, int transfer, float peak_in) {
    if (transfer == KOPAW_TF_UNKNOWN) return nonlin;
    vec3 lin = kopaw_eotf(nonlin, transfer);
    if (transfer == KOPAW_TF_PQ) {
        lin = kopaw_tonemap(lin, peak_in);
    } else if (transfer == KOPAW_TF_HLG) {
        // HLG 的 EOTF 已是相对值，参考峰值即 1.0。
        lin = kopaw_tonemap(lin, 1.0);
    }
    return kopaw_srgb_oetf(lin);
}
