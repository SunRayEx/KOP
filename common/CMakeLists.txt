# KOP 公共库：日志 / 单调时间 / SPSC 环形缓冲 / Vulkan 零拷贝装置
add_library(kop-common STATIC src/log.cpp src/time.cpp)
target_include_directories(kop-common PUBLIC ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(kop-common PRIVATE pthread)

# P3-M3：Vulkan 装置（KOPAW 导出器与 KOPMS 场景共用），随 Vulkan 后端开关。
if(KOPAW_BUILD_VULKAN)
    target_sources(kop-common PRIVATE src/vk_util.cpp)
    target_link_libraries(kop-common PUBLIC Vulkan::Vulkan)
endif()
