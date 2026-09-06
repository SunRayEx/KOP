// KOP App SDK 版本。与里程碑对齐：
//   0.3.x = P3-M3/M4（零拷贝 + Wayland 融合 + 输入管线）
#pragma once

#define KOP_APP_SDK_MAJOR 0
#define KOP_APP_SDK_MINOR 3
#define KOP_APP_SDK_PATCH 0
#define KOP_APP_SDK_VERSION "0.3.0"

namespace kop {
namespace sdk {

inline constexpr const char* version() { return KOP_APP_SDK_VERSION; }

}  // namespace sdk
}  // namespace kop
