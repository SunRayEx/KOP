// KOP App SDK —— 应用层统一入口。
//
// 面向应用/测试开发者的高层接口，屏蔽内部细节：
//   - kop::sdk::Pipeline        KOPAW 管线（媒体 → 解码 → 帧回调，无窗口）
//   - kop::sdk::FramePublisher  KOPMS 零拷贝发布（CPU 帧 → DMA-BUF → BUS）
//   - kop::sdk::TestReport      用户级测试报告（KOPAW_Test/KOPMS_Test 共用）
//
// 契约红线（与内部 ABI 一致，详见 docs/architecture.md）：
//   - 帧引用计数 + 独占释放；SDK 回调期间帧归管线；
//   - dma_buf_handle 不跨进程，跨进程走 FD 协议层（BUS2LAYER/SCM_RIGHTS）。
#pragma once

#define KOP_APP_SDK_INTERNAL 0

#include "kop/sdk/kopaw_pipeline.hpp"
#include "kop/sdk/kopms_publisher.hpp"
#include "kop/sdk/report.hpp"
#include "kop/sdk/version.hpp"

namespace kop {
namespace sdk {

// SDK 与宿主 KOPAW ABI 的匹配性检查（不匹配返回 false 并填 reason）。
bool check_abi(std::string* reason);

}  // namespace sdk
}  // namespace kop
