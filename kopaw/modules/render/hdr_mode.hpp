// HDR 输出模式与内容判定的共享定义。不依赖 Vulkan，供渲染后端接口、
// Vulkan 表面协商逻辑（vk_hdr.hpp）与播放器 CLI 共用，保证三处对
// “什么时候出 HDR”的判断只有一份实现。
#pragma once

#include <cstdint>
#include <string>

#include "kopaw_abi.h"  // KOPAW_COLOR_TRANSFER_*

namespace kopaw {

// 交换链 HDR 输出策略。
enum class HdrMode {
    Off = 0,  // 始终 sRGB SDR 输出（HDR 内容走 EOTF→色调映射→sRGB）
    Auto = 1, // 内容为 PQ/HLG 且表面支持 HDR10 时才启用
    On = 2,   // 表面支持时强制 HDR10 输出（SDR 内容按参考白抬升）
};

// "" / "auto" → Auto；"on"/"1"/"true" → On；"off"/"0"/"false" → Off。
// 无法识别时返回 Auto 并把原因写入 error（不阻断启动）。
inline HdrMode parse_hdr_mode(const std::string& s, std::string* error = nullptr) {
    if (s.empty() || s == "auto") return HdrMode::Auto;
    if (s == "on" || s == "1" || s == "true") return HdrMode::On;
    if (s == "off" || s == "0" || s == "false") return HdrMode::Off;
    if (error) *error = "未知 --hdr 值 '" + s + "'，按 auto 处理";
    return HdrMode::Auto;
}

// 内容侧是否为 HDR 传递函数（驱动交换链协商与着色器输出模式）。
inline bool content_is_hdr(int32_t transfer) {
    return transfer == KOPAW_COLOR_TRANSFER_PQ ||
           transfer == KOPAW_COLOR_TRANSFER_HLG;
}

// SDR 内容搬到 HDR10 输出时的参考白亮度（BT.2408 推荐 203 cd/m²）。
inline float sdr_reference_white_cd() { return 203.0f; }

}  // namespace kopaw
