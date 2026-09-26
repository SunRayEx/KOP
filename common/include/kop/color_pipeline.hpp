// 色彩管线契约：帧 ABI 的显式色彩元数据 → 片段着色器 push constant。
//
// 两个 Vulkan 消费端（KOPAW 渲染后端、KOPMS 合成场景）共用同一组 SPIR-V
// 片段着色器（shaders/color.glsl），因此 push constant 布局与“内容声明峰值”
// 的推导规则必须只有一份实现。未知 transfer 保持直通（旧路径），已知
// transfer 在着色器里走 EOTF；输出端两种模式：
//   * out_mode=0（SDR，默认）：HDR 色调映射 → sRGB OETF，输出缓冲约定为
//     sRGB 编码的 8-bit UNORM；
//   * out_mode=1（HDR10）：统一到绝对亮度 cd/m² → Rec.2020 原色 →
//     ST.2084 PQ OETF，输出缓冲约定为 A2B10G10R10 + HDR10_ST2084 交换链。
// 前 20 字节布局保持不变，SDR 消费端按原样填充即可。
#pragma once

#include <cstdint>

#include "kopaw_abi.h"

namespace kop {

// 输出模式（与 color.glsl 的 KOPAW_OUT_* 一致）。
inline constexpr int32_t kColorOutSdr = 0;
inline constexpr int32_t kColorOutHdr10 = 1;

// 片段着色器 push constant 布局（与 shaders/color.glsl 的 Push 一一对应）。
// bits10/matrix/range 只用于两平面 YUV 路径；transfer/peak_in 两路径共用。
// out_mode/primaries/sdr_ref 只用于 HDR10 输出路径，SDR 输出保持默认 0。
// 标量布局：8 个 4 字节槽，自然对齐，总 32 字节。
struct ColorPushConstants {
    int32_t bits10 = 0;    // 1 = P010（10bit 量化）
    int32_t matrix = 0;    // KOPAW_COLOR_MATRIX_*
    int32_t range = 0;     // KOPAW_COLOR_RANGE_*
    int32_t transfer = 0;  // KOPAW_COLOR_TRANSFER_*；UNKNOWN = 直通
    float peak_in = 0.0f;  // 内容声明峰值 cd/m²（PQ/HLG 色调映射锚点）
    int32_t out_mode = 0;  // 0 = SDR sRGB 输出；1 = HDR10 PQ 输出
    int32_t primaries = 0; // KOPAW_COLOR_PRIMARIES_*（内容侧，HDR10 原色转换依据）
    float sdr_ref = 0.0f;  // SDR 参考白 cd/m²；<=0 时着色器取 BT.2408 的 203
};
static_assert(sizeof(ColorPushConstants) == 32, "push constant 布局漂移");

// 内容声明峰值（cd/m²），用作 PQ/HLG 色调映射的锚点：
//   MaxCLL（内容峰值亮度）优先 → mastering display 的最大亮度次之
//   → 都没有时取 HDR 常规参考峰值 1000 cd/m²。
// ABI 单位：max_luminance 是 milli-cd/m²，max_cll/max_fall 是 cd/m²。
inline float declared_peak_luminance(const KopawColorMetadata& color) {
    if ((color.hdr.flags & KOPAW_HDR_FLAG_CONTENT_LIGHT) && color.hdr.max_cll > 0)
        return static_cast<float>(color.hdr.max_cll);
    if ((color.hdr.flags & KOPAW_HDR_FLAG_MASTERING_DISPLAY) &&
        color.hdr.max_luminance > 0)
        return static_cast<float>(color.hdr.max_luminance) / 1000.0f;
    return 1000.0f;
}

}  // namespace kop
