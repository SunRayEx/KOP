# KOP_AppSDK_Interface —— 应用层 SDK（KOPAW_Test / KOPMS_Test 及下游应用共用）
#
# 目标：KOP_AppSDK_Interface（静态库）
#   - kop::sdk::Pipeline        KOPAW 数据面（媒体 → 解码 → 帧回调）
#   - kop::sdk::FramePublisher  KOPMS 零拷贝发布（CPU 帧 → DMA-BUF → BUS）
#   - kop::sdk::NetTunnel       KOPNET 透明网络（逻辑通道，URI 驱动）
#   - kop::sdk::TestReport      用户级测试报告
# 下游接入：add_subdirectory(kop/sdk) 后链接 KOP_AppSDK_Interface，
# 或安装后 find_package / 直接引用 include/ 与库文件。
add_library(KOP_AppSDK_Interface STATIC
    src/app_sdk.cpp
    src/kopaw_pipeline.cpp
    src/kopms_publisher.cpp
    src/net_tunnel.cpp
)
add_library(kop::app-sdk ALIAS KOP_AppSDK_Interface)

target_include_directories(KOP_AppSDK_Interface PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(KOP_AppSDK_Interface PUBLIC
    kopaw-modules      # 节点实现（解复用/解码/导出器）+ kop-common + FFmpeg
    kopaw_core         # Rust 图引擎（kopaw_graph_* C ABI）
    kopms-protocol     # BUS2LAYER 客户端 + 控制面
    kopnet             # 透明网络：隧道/协议适配器/韧性层
)
set_target_properties(KOP_AppSDK_Interface PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(BUILD_TESTING)
    # SDK facade 冒烟：NetTunnel 双向通信 / 多通道 / 重连 / 契约校验
    add_executable(kopnet-sdk-test tests/net_tunnel_test.cpp)
    target_link_libraries(kopnet-sdk-test PRIVATE kop::app-sdk pthread)
    add_test(NAME kopnet-sdk-test COMMAND kopnet-sdk-test)
endif()
