#include "kop/app_sdk.hpp"

#include "kopaw_abi.h"

namespace kop {
namespace sdk {

// SDK 编译期绑定的 ABI 契约必须与宿主引擎一致：
//   - 主版本不同 → 布局/所有权语义不兼容；
//   - 插件/宿主次版本协商规则：SDK 次版本不得高于引擎。
bool check_abi(std::string* reason) {
    const KopawAbiInfo info = kopaw_abi_info();
    if (info.struct_size < sizeof(KopawAbiInfo)) {
        if (reason) {
            *reason = "KopawAbiInfo.struct_size 过小（" + std::to_string(info.struct_size) +
                      " < " + std::to_string(sizeof(KopawAbiInfo)) + "）";
        }
        return false;
    }
    if (info.major != KOPAW_ABI_MAJOR) {
        if (reason) {
            *reason = "KOPAW ABI 主版本不匹配（宿主 " + std::to_string(info.major) +
                      " vs SDK " + std::to_string(KOPAW_ABI_MAJOR) + "）";
        }
        return false;
    }
    if (info.minor > KOPAW_ABI_MINOR) {
        if (reason) {
            *reason = "KOPAW ABI 次版本过新（宿主 " + std::to_string(info.minor) +
                      " > SDK " + std::to_string(KOPAW_ABI_MINOR) + "）";
        }
        return false;
    }
    if (reason) {
        *reason = "KOPAW ABI " + std::to_string(info.major) + "." +
                  std::to_string(info.minor) + "，能力位 0x" +
                  std::to_string(info.capabilities);
    }
    return true;
}

}  // namespace sdk
}  // namespace kop
