# KOPMS：Wayland 兼容层 + KOPMS-S/KOPMS-C BUS2LAYER 协议

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)
pkg_check_modules(LIBDRM QUIET IMPORTED_TARGET libdrm)
pkg_check_modules(LIBINPUT QUIET IMPORTED_TARGET libinput)
pkg_check_modules(LIBUDEV QUIET IMPORTED_TARGET libudev)
pkg_check_modules(LIBSYSTEMD QUIET IMPORTED_TARGET libsystemd)
pkg_check_modules(XKBCOMMON QUIET IMPORTED_TARGET xkbcommon)

set(KOPMS_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${KOPMS_PROTO_DIR})
set(XDG_XML /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml)

add_custom_command(
    OUTPUT ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c
           ${KOPMS_PROTO_DIR}/xdg-shell-server-protocol.h
           ${KOPMS_PROTO_DIR}/xdg-shell-client-protocol.h
    COMMAND wayland-scanner private-code ${XDG_XML}
            ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c
    COMMAND wayland-scanner server-header ${XDG_XML}
            ${KOPMS_PROTO_DIR}/xdg-shell-server-protocol.h
    COMMAND wayland-scanner client-header ${XDG_XML}
            ${KOPMS_PROTO_DIR}/xdg-shell-client-protocol.h
    DEPENDS ${XDG_XML}
    VERBATIM)

# P3-M4：linux-dmabuf（格式/modifier 协商）与 explicit synchronization。
# 系统 wayland-protocols 缺失时跳过（nested 协议验证路径仍可用）。
set(KOPMS_DMABUF_XML
    /usr/share/wayland-protocols/stable/linux-dmabuf/linux-dmabuf-v1.xml)
set(KOPMS_EXPSYNC_XML
    /usr/share/wayland-protocols/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml)
if(EXISTS ${KOPMS_DMABUF_XML})
    add_custom_command(
        OUTPUT ${KOPMS_PROTO_DIR}/linux-dmabuf-protocol.c
               ${KOPMS_PROTO_DIR}/linux-dmabuf-server-protocol.h
               ${KOPMS_PROTO_DIR}/linux-dmabuf-client-protocol.h
        COMMAND wayland-scanner private-code ${KOPMS_DMABUF_XML}
                ${KOPMS_PROTO_DIR}/linux-dmabuf-protocol.c
        COMMAND wayland-scanner server-header ${KOPMS_DMABUF_XML}
                ${KOPMS_PROTO_DIR}/linux-dmabuf-server-protocol.h
        COMMAND wayland-scanner client-header ${KOPMS_DMABUF_XML}
                ${KOPMS_PROTO_DIR}/linux-dmabuf-client-protocol.h
        DEPENDS ${KOPMS_DMABUF_XML}
        VERBATIM)
    set(KOPMS_HAVE_LINUX_DMABUF_PROTO 1)
else()
    message(STATUS "KOPMS linux-dmabuf protocol disabled (XML not found)")
endif()
if(EXISTS ${KOPMS_EXPSYNC_XML})
    add_custom_command(
        OUTPUT ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-protocol.c
               ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-server-protocol.h
               ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-client-protocol.h
        COMMAND wayland-scanner private-code ${KOPMS_EXPSYNC_XML}
                ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-protocol.c
        COMMAND wayland-scanner server-header ${KOPMS_EXPSYNC_XML}
                ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-server-protocol.h
        COMMAND wayland-scanner client-header ${KOPMS_EXPSYNC_XML}
                ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-client-protocol.h
        DEPENDS ${KOPMS_EXPSYNC_XML}
        VERBATIM)
    set(KOPMS_HAVE_EXPLICIT_SYNC_PROTO 1)
else()
    message(STATUS "KOPMS explicit-sync protocol disabled (XML not found)")
endif()

add_executable(kopms-compositor
    compositor/compositor.cpp
    compositor/nested_output.cpp
    compositor/dmabuf_bridge.cpp
    compositor/input_state.cpp
    ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c
)
if(XKBCOMMON_FOUND)
    target_link_libraries(kopms-compositor PRIVATE PkgConfig::XKBCOMMON)
    target_compile_definitions(kopms-compositor PRIVATE KOPMS_HAVE_XKBCOMMON=1)
    message(STATUS "KOPMS keyboard keymap (xkbcommon) enabled")
endif()
if(KOPMS_HAVE_LINUX_DMABUF_PROTO)
    target_sources(kopms-compositor PRIVATE ${KOPMS_PROTO_DIR}/linux-dmabuf-protocol.c)
    target_compile_definitions(kopms-compositor PRIVATE KOPMS_HAVE_LINUX_DMABUF=1)
endif()
if(KOPMS_HAVE_EXPLICIT_SYNC_PROTO)
    target_sources(kopms-compositor PRIVATE
        ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-protocol.c)
    target_compile_definitions(kopms-compositor PRIVATE KOPMS_HAVE_EXPLICIT_SYNC=1)
endif()
target_include_directories(kopms-compositor PRIVATE
    ${KOPMS_PROTO_DIR} ${CMAKE_SOURCE_DIR}/common/include)
add_library(kopms-platform STATIC
    platform/drm_kms.cpp
    platform/drm_buffer.cpp
    platform/drm_hotplug.cpp
    platform/drm_output.cpp
    platform/gpu_mode.cpp
    platform/libinput_guard.cpp
    platform/seat_session.cpp
    platform/frame_bridge.cpp
)
target_include_directories(kopms-platform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
    ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(kopms-platform PUBLIC kop-common)
if(LIBDRM_FOUND)
    target_compile_definitions(kopms-platform PRIVATE KOPMS_HAVE_LIBDRM=1)
    target_link_libraries(kopms-platform PUBLIC PkgConfig::LIBDRM)
    message(STATUS "KOPMS DRM/KMS probe enabled")
else()
    message(STATUS "KOPMS DRM/KMS probe disabled (libdrm not found)")
endif()

if(LIBUDEV_FOUND)
    target_compile_definitions(kopms-platform PRIVATE KOPMS_HAVE_LIBUDEV=1)
    target_link_libraries(kopms-platform PUBLIC PkgConfig::LIBUDEV)
    message(STATUS "KOPMS DRM hotplug monitor enabled")
else()
    message(STATUS "KOPMS DRM hotplug monitor disabled (libudev not found)")
endif()

# P3-M3/M4：Vulkan GPU 合成场景（DMA-BUF 导入 + 多窗口合成 + 回压）。
option(KOPMS_BUILD_VK_SCENE "构建 KOPMS Vulkan 场景合成器" ON)
if(KOPMS_BUILD_VK_SCENE AND KOPAW_BUILD_VULKAN)
    # 场景复用 KOPAW 的全屏四边形着色器（构建期编译并嵌入 SPIR-V）
    set(KOPMS_SCENE_SHADER_DIR ${CMAKE_SOURCE_DIR}/kopaw/modules/render/shaders)
    set(KOPMS_SCENE_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${KOPMS_SCENE_GEN_DIR})
    add_custom_command(
        OUTPUT ${KOPMS_SCENE_GEN_DIR}/scene_shaders_embed.cpp
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=$<TARGET_FILE:glslang-standalone>
            -DSHADER_DIR=${KOPMS_SCENE_SHADER_DIR}
            -DOUT_FILE=${KOPMS_SCENE_GEN_DIR}/scene_shaders_embed.cpp
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedShaders.cmake
        DEPENDS glslang-standalone
                ${KOPMS_SCENE_SHADER_DIR}/quad.vert ${KOPMS_SCENE_SHADER_DIR}/quad.frag
        VERBATIM
    )
    target_sources(kopms-platform PRIVATE
        platform/vk_scene.cpp
        ${KOPMS_SCENE_GEN_DIR}/scene_shaders_embed.cpp
    )
    target_compile_definitions(kopms-platform PUBLIC KOPMS_SCENE_WINDOW=1)
    target_link_libraries(kopms-platform PUBLIC Vulkan::Vulkan glfw)
    message(STATUS "KOPMS Vulkan scene compositor enabled")
else()
    message(STATUS "KOPMS Vulkan scene compositor disabled")
endif()

if(LIBSYSTEMD_FOUND)
    target_compile_definitions(kopms-platform PRIVATE KOPMS_HAVE_SYSTEMD=1)
    target_link_libraries(kopms-platform PUBLIC PkgConfig::LIBSYSTEMD)
    message(STATUS "KOPMS systemd-logind seat backend enabled")
else()
    message(STATUS "KOPMS systemd-logind seat backend disabled")
endif()

add_library(kopms-bus2layer STATIC
    bus2layer/bus2layer.cpp
)
target_include_directories(kopms-bus2layer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/bus2layer
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
)

add_library(kopms-protocol STATIC
    control/control_state.cpp
    protocol/kopms_protocol.cpp
    protocol/kopms_server.cpp
    protocol/kopms_client.cpp
)
target_include_directories(kopms-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/control
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/bus2layer
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
)
target_link_libraries(kopms-protocol PUBLIC
    kopms-bus2layer kopms-platform wayland-server)
if(LIBINPUT_FOUND)
    target_compile_definitions(kopms-platform PRIVATE KOPMS_HAVE_LIBINPUT=1)
    target_link_libraries(kopms-platform PRIVATE PkgConfig::LIBINPUT)
    message(STATUS "KOPMS libinput guard enabled")
else()
    message(STATUS "KOPMS libinput guard disabled (libinput not found)")
endif()

target_link_libraries(kopms-compositor PRIVATE
    wayland-server OpenGL::GL glfw kop-common kopms-platform kopms-protocol)

add_executable(kopms-test-client clients/test_client.cpp ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c)
target_include_directories(kopms-test-client PRIVATE ${KOPMS_PROTO_DIR})
target_link_libraries(kopms-test-client PRIVATE wayland-client)

add_executable(kopms-client clients/kopms_client.cpp)
target_link_libraries(kopms-client PRIVATE kopms-protocol)

# 输入反馈客户端：渲染点击标记/键入字符（GUI 反馈可视化）
add_executable(kopms-input-client clients/input_client.cpp
    ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c)
target_include_directories(kopms-input-client PRIVATE
    ${KOPMS_PROTO_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/compositor)
target_link_libraries(kopms-input-client PRIVATE wayland-client)

if(BUILD_TESTING)
    add_executable(kopms-gpu-mode-test tests/gpu_mode_test.cpp)
    target_link_libraries(kopms-gpu-mode-test PRIVATE kopms-platform)
    add_test(NAME kopms-gpu-mode COMMAND kopms-gpu-mode-test)

    add_executable(kopms-frame-bridge-test tests/frame_bridge_test.cpp)
    target_link_libraries(kopms-frame-bridge-test PRIVATE kopms-platform)
    add_test(NAME kopms-frame-bridge COMMAND kopms-frame-bridge-test)

    add_executable(kopms-drm-kms-test tests/drm_kms_test.cpp)
    target_link_libraries(kopms-drm-kms-test PRIVATE kopms-platform)
    add_test(NAME kopms-drm-kms COMMAND kopms-drm-kms-test)

    add_executable(kopms-drm-buffer-test tests/drm_buffer_test.cpp)
    target_link_libraries(kopms-drm-buffer-test PRIVATE kopms-platform)
    add_test(NAME kopms-drm-buffer COMMAND kopms-drm-buffer-test)

    add_executable(kopms-seat-session-test tests/seat_session_test.cpp)
    target_link_libraries(kopms-seat-session-test PRIVATE kopms-platform)
    add_test(NAME kopms-seat-session COMMAND kopms-seat-session-test)

    add_executable(kopms-drm-hotplug-test tests/drm_hotplug_test.cpp)
    target_link_libraries(kopms-drm-hotplug-test PRIVATE kopms-platform)
    add_test(NAME kopms-drm-hotplug COMMAND kopms-drm-hotplug-test)

    add_executable(kopms-drm-output-test tests/drm_output_test.cpp)
    target_link_libraries(kopms-drm-output-test PRIVATE kopms-platform)
    add_test(NAME kopms-drm-output COMMAND kopms-drm-output-test)

    add_executable(kopms-bus-test tests/bus_test.cpp)
    target_link_libraries(kopms-bus-test PRIVATE kopms-protocol)
    add_test(NAME kopms-bus COMMAND kopms-bus-test)

    add_executable(kopms-m34-protocol-test tests/m34_protocol_test.cpp)
    target_link_libraries(kopms-m34-protocol-test PRIVATE kopms-protocol)
    add_test(NAME kopms-m34-protocol COMMAND kopms-m34-protocol-test)

    if(KOPMS_BUILD_VK_SCENE AND KOPAW_BUILD_VULKAN)
        # P3-M3 核心链路：Vulkan 导出 → 跨设备 DMA-BUF 导入 → 像素回环。
        # 无 DMA-BUF 能力的环境（CI）以退出码 77 跳过。
        add_executable(kopms-vk-dmabuf-test tests/vk_dmabuf_roundtrip_test.cpp
            ${CMAKE_SOURCE_DIR}/kopaw/modules/render/vulkan/vk_dma_export.cpp)
        target_include_directories(kopms-vk-dmabuf-test PRIVATE
            ${CMAKE_SOURCE_DIR}/kopaw/modules
            ${CMAKE_SOURCE_DIR}/common/include
            ${CMAKE_SOURCE_DIR}/kopaw/abi/include)
        target_link_libraries(kopms-vk-dmabuf-test PRIVATE kop-common Vulkan::Vulkan)
        add_test(NAME kopms-vk-dmabuf-roundtrip COMMAND kopms-vk-dmabuf-test)
        set_tests_properties(kopms-vk-dmabuf-roundtrip PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 60)

        # P3-M3/M4 基准（手动运行；不进 ctest）。
        add_executable(kopms-dmabuf-bench tests/dmabuf_bench.cpp
            ${CMAKE_SOURCE_DIR}/kopaw/modules/render/vulkan/vk_dma_export.cpp)
        target_link_libraries(kopms-dmabuf-bench PRIVATE
            kopms-protocol kop-common Vulkan::Vulkan glfw)
        target_include_directories(kopms-dmabuf-bench PRIVATE
            ${CMAKE_SOURCE_DIR}/kopaw/modules
            ${CMAKE_SOURCE_DIR}/common/include
            ${CMAKE_SOURCE_DIR}/kopaw/abi/include)

        # P3-M4 Wayland 兼容面端到端：真实客户端进程走 linux-dmabuf /
        # explicit-sync → 场景导入 → release。需要显示环境（无则以 77 跳过）。
        add_executable(kopms-dmabuf-client-test tests/dmabuf_wayland_client_test.cpp
            ${KOPMS_PROTO_DIR}/xdg-shell-protocol.c
            ${KOPMS_PROTO_DIR}/linux-dmabuf-protocol.c
            ${KOPMS_PROTO_DIR}/linux-explicit-synchronization-protocol.c
            ${CMAKE_SOURCE_DIR}/kopaw/modules/render/vulkan/vk_dma_export.cpp)
        target_include_directories(kopms-dmabuf-client-test PRIVATE
            ${KOPMS_PROTO_DIR}
            ${CMAKE_SOURCE_DIR}/kopaw/modules
            ${CMAKE_SOURCE_DIR}/common/include
            ${CMAKE_SOURCE_DIR}/kopaw/abi/include)
        target_link_libraries(kopms-dmabuf-client-test PRIVATE
            wayland-client kop-common Vulkan::Vulkan)
        add_test(NAME kopms-dmabuf-client COMMAND kopms-dmabuf-client-test)
        set_tests_properties(kopms-dmabuf-client PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 60)

        # 输入/输出持久压测：合成器 soak 自驱动 → 反馈客户端渲染 → 计数核对。
        add_executable(kopms-input-soak-test tests/input_soak_test.cpp)
        target_include_directories(kopms-input-soak-test PRIVATE
            ${CMAKE_SOURCE_DIR}/common/include)
        add_test(NAME kopms-input-soak COMMAND kopms-input-soak-test)
        set_tests_properties(kopms-input-soak PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 90)
    endif()

    add_test(NAME kopms-protocol-smoke
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_smoke.sh
                $<TARGET_FILE:kopms-compositor>
                $<TARGET_FILE:kopms-test-client>
                $<TARGET_FILE:kopms-client>)
    set_tests_properties(kopms-protocol-smoke PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 15)
endif()

if(KOPAW_BUILD_OPENGL)
    # 嵌套输出需要 GL
    add_dependencies(kopms-compositor glfw)
endif()
