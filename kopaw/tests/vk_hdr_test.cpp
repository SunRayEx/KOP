// HDR 显示能力协商与 HDR10 编码数学的单元测试。不依赖 GPU 或显示器：
//   * select_surface_output / surface_supports_hdr10：合成表面格式表覆盖
//     Off/Auto/On × HDR10 可用/不可用 × 内容 HDR/SDR 全部分支；
//   * parse_hdr_mode / content_is_hdr：CLI 与内容判定；
//   * ST.2084 OETF 与 EOTF 互为严格逆运算（CPU 双胞胎实现）；
//   * BT.709 / Display-P3 → BT.2020 矩阵从色度坐标独立推导，作为
//     color.glsl 中同名常量的 oracle（shader 侧转录由播放器实测兜底）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "render/hdr_mode.hpp"
#include "render/vulkan/vk_hdr.hpp"

#include "kop/log.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what, double a = 0.0, double b = 0.0) {
    if (ok) return;
    ++g_failures;
    KOP_LOG_ERROR("vk-hdr-test", "FAIL %s (%g vs %g)", what, a, b);
}

// ---- ST.2084 CPU 双胞胎（与 color.glsl 数值逐位一致）---------------------
double pq_eotf1(double v) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    const double em = std::pow(v, 1.0 / m2);
    return 10000.0 * std::pow(std::max(em - c1, 0.0) / (c2 - c3 * em), 1.0 / m1);
}

double pq_oetf1(double l_cd) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 2413.0 / 4096.0 * 32.0;
    const double c3 = 2392.0 / 4096.0 * 32.0;
    const double l = std::pow(std::min(std::max(l_cd, 0.0), 10000.0) / 10000.0, m1);
    return std::pow((c1 + c2 * l) / (1.0 + c3 * l), m2);
}

// ---- 3x3 线性代数 -------------------------------------------------------
struct Mat3 {
    double m[3][3]{};
};

Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// 高斯消元解 M·x = rhs（原地修改入参）。
bool mat_solve(Mat3 a, double rhs[3], double x[3]) {
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::abs(a.m[r][col]) > std::abs(a.m[piv][col])) piv = r;
        if (std::abs(a.m[piv][col]) < 1e-12) return false;
        for (int c = 0; c < 3; ++c) std::swap(a.m[col][c], a.m[piv][c]);
        std::swap(rhs[col], rhs[piv]);
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = a.m[r][col] / a.m[col][col];
            for (int c = 0; c < 3; ++c) a.m[r][c] -= f * a.m[col][c];
            rhs[r] -= f * rhs[col];
        }
    }
    for (int r = 0; r < 3; ++r) x[r] = rhs[r] / a.m[r][r];
    return true;
}

Mat3 mat_inv(const Mat3& a) {
    auto cof = [&](int i, int j) {
        const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
        const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
        return a.m[i1][j1] * a.m[i2][j2] - a.m[i1][j2] * a.m[i2][j1];
    };
    double det = 0.0;
    for (int j = 0; j < 3; ++j) det += a.m[0][j] * cof(0, j);
    Mat3 inv;
    if (std::abs(det) < 1e-12) return inv;
    const double inv_det = 1.0 / det;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) inv.m[j][i] = cof(i, j) * inv_det;
    return inv;
}

// 色度坐标 → CIE XYZ 三刺激值。
void xy_to_xyz(double x, double y, double out[3]) {
    const double d = (y > 1e-9) ? y : 1e-9;
    out[0] = x / d;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / d;
}

// 原色 + 白点 → RGB→XYZ 矩阵（列缩放到白点）。
Mat3 rgb_to_xyz(const double p[6], double xw, double yw) {
    double R[3], G[3], B[3], W[3];
    xy_to_xyz(p[0], p[1], R);
    xy_to_xyz(p[2], p[3], G);
    xy_to_xyz(p[4], p[5], B);
    xy_to_xyz(xw, yw, W);
    Mat3 m;  // 列 = R,G,B
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = R[i];
        m.m[i][1] = G[i];
        m.m[i][2] = B[i];
    }
    double s[3]{};
    if (!mat_solve(m, W, s)) return m;
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = m.m[i][j] * s[j];
    return r;
}

// 源原色 → 目标原色的线性 RGB→RGB 矩阵。
Mat3 convert_primaries(const double src[6], const double dst[6], double xw,
                       double yw) {
    return mat_mul(mat_inv(rgb_to_xyz(dst, xw, yw)), rgb_to_xyz(src, xw, yw));
}

const double kD65x = 0.3127, kD65y = 0.3290;
const double kP709[6] = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
const double kP2020[6] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
const double kPP3[6] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};

// shader 中的常量（color.glsl 的 kopaw_primaries_to_2020，行主序）。
const double k709To2020[9] = {0.6274040, 0.3292830, 0.0433130,
                              0.0690970, 0.9195410, 0.0113620,
                              0.0163910, 0.0880130, 0.8955960};
const double kP3To2020[9] = {0.7538330, 0.1985974, 0.0475696,
                             0.0457438, 0.9417772, 0.0124789,
                             -0.0012103, 0.0176017, 0.9836086};

}  // namespace

int main() {
    using kopaw::HdrMode;
    using kopaw::SurfaceOutput;

    // ---- surface_supports_hdr10 ------------------------------------------
    {
        const VkSurfaceFormatKHR sdr_only[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        check(!kopaw::surface_supports_hdr10(sdr_only, 1),
              "surface_supports_hdr10 sdr-only");
        const VkSurfaceFormatKHR with_hdr[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        };
        check(kopaw::surface_supports_hdr10(with_hdr, 2),
              "surface_supports_hdr10 hdr");
        // 只有 10-bit 格式但不是 ST.2084 色彩空间（合成器常见的 10-bit SDR）
        const VkSurfaceFormatKHR tenbit_sdr[] = {
            {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        check(!kopaw::surface_supports_hdr10(tenbit_sdr, 1),
              "surface_supports_hdr10 rejects 10-bit SDR");
        check(!kopaw::surface_supports_hdr10(nullptr, 0),
              "surface_supports_hdr10 empty");
    }

    // ---- select_surface_output -------------------------------------------
    {
        const VkSurfaceFormatKHR sdr_only[] = {
            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        const VkSurfaceFormatKHR with_hdr[] = {
            {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
            {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
        };

        SurfaceOutput o;

        // Off 永远 SDR，即使表面支持、内容是 HDR。
        o = kopaw::select_surface_output(with_hdr, 2, HdrMode::Off, true);
        check(!o.hdr10 && o.color_space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
              "Off stays SDR");
        check(o.format == VK_FORMAT_R8G8B8A8_UNORM, "Off picks UNORM");

        // On：表面支持 → HDR10。
        o = kopaw::select_surface_output(with_hdr, 2, HdrMode::On, false);
        check(o.hdr10 && o.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                  o.color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT,
              "On selects HDR10");

        // On：表面不支持 → 回退 SDR。
        o = kopaw::select_surface_output(sdr_only, 1, HdrMode::On, true);
        check(!o.hdr10, "On falls back to SDR");

        // Auto：SDR 内容 → SDR（哪怕表面支持）。
        o = kopaw::select_surface_output(with_hdr, 2, HdrMode::Auto, false);
        check(!o.hdr10, "Auto + SDR content stays SDR");

        // Auto：HDR 内容 + 支持 → HDR10。
        o = kopaw::select_surface_output(with_hdr, 2, HdrMode::Auto, true);
        check(o.hdr10, "Auto + HDR content selects HDR10");

        // Auto：HDR 内容 + 不支持 → SDR。
        o = kopaw::select_surface_output(sdr_only, 1, HdrMode::Auto, true);
        check(!o.hdr10, "Auto + HDR content falls back to SDR");

        // 空表面表：安全 SDR 默认（不崩）。
        o = kopaw::select_surface_output(nullptr, 0, HdrMode::On, true);
        check(!o.hdr10 && o.format == VK_FORMAT_B8G8R8A8_UNORM,
              "empty surface defaults to SDR");

        // 无首选 UNORM+sRGB 时取驱动首选项。
        const VkSurfaceFormatKHR odd[] = {
            {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        };
        o = kopaw::select_surface_output(odd, 1, HdrMode::Off, false);
        check(!o.hdr10 && o.format == VK_FORMAT_R5G6B5_UNORM_PACK16,
              "falls back to driver first choice");
    }

    // ---- parse_hdr_mode ---------------------------------------------------
    {
        std::string e;
        check(kopaw::parse_hdr_mode("", &e) == HdrMode::Auto, "parse empty");
        check(kopaw::parse_hdr_mode("auto", &e) == HdrMode::Auto, "parse auto");
        check(kopaw::parse_hdr_mode("on", &e) == HdrMode::On, "parse on");
        check(kopaw::parse_hdr_mode("1", &e) == HdrMode::On, "parse 1");
        check(kopaw::parse_hdr_mode("true", &e) == HdrMode::On, "parse true");
        check(kopaw::parse_hdr_mode("off", &e) == HdrMode::Off, "parse off");
        check(kopaw::parse_hdr_mode("0", &e) == HdrMode::Off, "parse 0");
        check(kopaw::parse_hdr_mode("false", &e) == HdrMode::Off, "parse false");
        check(kopaw::parse_hdr_mode("bogus", &e) == HdrMode::Auto && !e.empty(),
              "parse unknown -> Auto with message");
    }

    // ---- content_is_hdr ---------------------------------------------------
    check(kopaw::content_is_hdr(KOPAW_COLOR_TRANSFER_PQ), "PQ is HDR");
    check(kopaw::content_is_hdr(KOPAW_COLOR_TRANSFER_HLG), "HLG is HDR");
    check(!kopaw::content_is_hdr(KOPAW_COLOR_TRANSFER_BT709), "709 is not HDR");
    check(!kopaw::content_is_hdr(KOPAW_COLOR_TRANSFER_SRGB), "sRGB is not HDR");
    check(!kopaw::content_is_hdr(KOPAW_COLOR_TRANSFER_UNKNOWN),
          "UNKNOWN is not HDR");
    check(kopaw::sdr_reference_white_cd() == 203.0, "SDR reference white 203");

    // ---- ST.2084 互逆性 ---------------------------------------------------
    {
        const double values[] = {0.001, 0.01, 0.1, 0.25, 0.5,  0.5807,
                                 0.75,  0.9,  0.99, 1.0};
        double worst = 0.0;
        for (double v : values)
            worst = std::max(worst, std::abs(pq_oetf1(pq_eotf1(v)) - v));
        check(worst < 1e-9, "PQ OETF is strict inverse of EOTF", worst, 1e-9);
    }
    // ---- ST.2084 绝对编码锚点 --------------------------------------------
    {
        check(pq_oetf1(0.0) < 1e-6, "0 cd -> code ~0");
        check(std::abs(pq_oetf1(10000.0) - 1.0) < 1e-9, "10000 cd -> code 1");
        // ITU 参考值：100 cd/m² 约 0.508；BT.2408 SDR 参考白 203 cd/m² 约 0.581。
        const double c100 = pq_oetf1(100.0);
        check(std::abs(c100 - 0.5080784) < 1e-4, "100 cd ~ 0.508 PQ", c100,
              0.5080784);
        const double c203 = pq_oetf1(203.0);
        check(std::abs(c203 - 0.5806889) < 1e-4, "203 cd ~ 0.581 PQ", c203,
              0.5806889);
        // 内容峰值 1000 cd/m² 的 PQ 码值，HDR10 色调映射的常见锚点。
        const double c1000 = pq_oetf1(1000.0);
        check(std::abs(c1000 - 0.7518271) < 1e-4, "1000 cd ~ 0.752 PQ", c1000,
              0.7518271);
    }

    // ---- 原色矩阵：从色度坐标推导并比对 shader 常量 ------------------------
    {
        const Mat3 m709 =
            convert_primaries(kP709, kP2020, kD65x, kD65y);
        const Mat3 mp3 = convert_primaries(kPP3, kP2020, kD65x, kD65y);
        const Mat3 m2020 =
            convert_primaries(kP2020, kP2020, kD65x, kD65y);  // 恒等
        double worst709 = 0.0, worstP3 = 0.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                worst709 = std::max(
                    worst709, std::abs(m709.m[i][j] - k709To2020[i * 3 + j]));
                worstP3 =
                    std::max(worstP3, std::abs(mp3.m[i][j] - kP3To2020[i * 3 + j]));
            }
        check(worst709 < 1e-4, "BT.709 -> BT.2020 matches derivation", worst709,
              1e-4);
        check(worstP3 < 1e-4, "Display-P3 -> BT.2020 matches derivation",
              worstP3, 1e-4);
        // BT.2020→BT.2020 恒等：原色相同时矩阵是单位阵（白点一致）。
        double worstId = 0.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                worstId = std::max(
                    worstId, std::abs(m2020.m[i][j] - (i == j ? 1.0 : 0.0)));
        check(worstId < 1e-9, "BT.2020 identity primaries", worstId, 1e-9);
    }

    if (g_failures == 0) {
        KOP_LOG_INFO("vk-hdr-test", "ALL PASS");
        return 0;
    }
    KOP_LOG_ERROR("vk-hdr-test", "%d 项断言失败", g_failures);
    return 1;
}
