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

// KOPAW_COLOR_PRIMARIES_*（kopaw_abi.h）
#define KOPAW_P_UNKNOWN 0
#define KOPAW_P_BT601   1
#define KOPAW_P_BT709   2
#define KOPAW_P_BT2020  3
#define KOPAW_P_P3      4

// 输出模式
#define KOPAW_OUT_SDR   0  // sRGB 编码 8-bit UNORM
#define KOPAW_OUT_HDR10 1  // PQ 编码 Rec.2020（A2B10G10R10 + HDR10_ST2084）

// SDR 内容搬到 HDR10 输出时的参考白（BT.2408 推荐 203 cd/m²）。
#define KOPAW_SDR_REF_CD 203.0

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

// ST.2084 OETF（EOTF 的严格逆）：线性亮度 cd/m² → 非线性编码值 [0,1]。
// 与 kopaw_eotf1 的 PQ 分支互为逆运算（先升 m1 次幂是关键）。
float kopaw_pq_oetf1(float l_cd) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float l = pow(clamp(l_cd, 0.0, 10000.0) / 10000.0, m1);
    return pow((c1 + c2 * l) / (1.0 + c3 * l), m2);
}

vec3 kopaw_pq_oetf(vec3 l_cd) {
    return vec3(kopaw_pq_oetf1(l_cd.r), kopaw_pq_oetf1(l_cd.g),
                kopaw_pq_oetf1(l_cd.b));
}

// 内容原色 → Rec.2020 的线性矩阵（两者都以 D65 为白点，故是 3x3 旋转）。
// BT.2020 内容恒等；Display-P3 与 BT.709/BT.601（近似）各自展开到 2020。
// 数值与 color_pipeline_test 的色度坐标推导结果逐项比对过。
mat3 kopaw_primaries_to_2020(int primaries) {
    if (primaries == KOPAW_P_BT2020) {
        return mat3(1.0);
    }
    if (primaries == KOPAW_P_P3) {
        // Display-P3（D65）→ Rec.2020
        return mat3(0.7538330, 0.0457438, -0.0012103,
                    0.1985974, 0.9417772, 0.0176017,
                    0.0475696, 0.0124789, 0.9836086);
    }
    // BT.709（BT.601 近似）→ Rec.2020
    return mat3(0.6274040, 0.0690970, 0.0163910,
                0.3292830, 0.9195410, 0.0880130,
                0.0433130, 0.0113620, 0.8955960);
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

// 完整链（含 HDR10 输出）：out_mode=KOPAW_OUT_HDR10 时统一到绝对亮度 cd/m²
// （PQ 已是绝对值；HLG 乘名义峰 1000；SDR 传递函数乘参考白），过原色矩阵到
// Rec.2020，再 PQ 编码。HDR10 是绝对编码：应用侧不做色调映射，高光收敛交给
// 显示器自身的 EOTF/色调映射，这是标准 HDR10 行为。
vec3 kopaw_apply_color_out(vec3 nonlin, int transfer, float peak_in,
                          int out_mode, int primaries, float sdr_ref) {
    if (out_mode != KOPAW_OUT_HDR10)
        return kopaw_apply_color(nonlin, transfer, peak_in);
    if (sdr_ref <= 0.0) sdr_ref = KOPAW_SDR_REF_CD;
    if (transfer == KOPAW_TF_UNKNOWN) {
        // 未声明内容按 SDR 参考白抬升后进 HDR10。
        vec3 abs_lin = nonlin * sdr_ref;
        return kopaw_pq_oetf(kopaw_primaries_to_2020(primaries) * abs_lin);
    }
    vec3 lin = kopaw_eotf(nonlin, transfer);
    vec3 abs_lin;
    if (transfer == KOPAW_TF_PQ) {
        abs_lin = lin;                   // EOTF 输出已是 cd/m²
    } else if (transfer == KOPAW_TF_HLG) {
        abs_lin = lin * 1000.0;          // 相对亮度，名义峰 1000 cd/m²
    } else {
        abs_lin = lin * sdr_ref;         // SDR 相对亮度 → 绝对值
    }
    return kopaw_pq_oetf(kopaw_primaries_to_2020(primaries) * abs_lin);
}
