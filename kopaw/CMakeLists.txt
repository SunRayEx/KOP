# KOPAW：管线核心（Rust，经 Corrosion）+ C++ 节点库 + CLI 播放器

# Rust 图引擎（kopaw-core staticlib，导出 kopaw_* C ABI）
corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/core/Cargo.toml
    CRATE_TYPES staticlib)

# ---------------------------------------------------------------------------
# C++ 节点库
# ---------------------------------------------------------------------------
set(KOPAW_MODULES_SOURCES
    modules/ffmpeg/demuxer_node.cpp
    modules/ffmpeg/video_decoder_node.cpp
    modules/ffmpeg/audio_decoder_node.cpp
    modules/ffmpeg/filter_node.cpp
    modules/audio/portaudio_sink_node.cpp
    modules/render/render_backend.cpp
    modules/render/render_node.cpp
    modules/plugin_loader.cpp
)

set(KOPAW_HAVE_LIBVA_VAL 0)
if(KOPAW_BUILD_VULKAN)
    list(APPEND KOPAW_MODULES_SOURCES
        modules/render/vulkan/vulkan_backend.cpp
        modules/render/vulkan/vk_ycbcr.cpp
        modules/render/vulkan/vk_dma_export.cpp
        modules/ffmpeg/hwaccel.cpp)
    # P2 硬解零拷贝：VAAPI 表面原生 DMA-BUF 导出（libva 可选依赖）
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBVA QUIET IMPORTED_TARGET libva)
    endif()
    if(LIBVA_FOUND)
        set(KOPAW_HAVE_LIBVA_VAL 1)
        list(APPEND KOPAW_MODULES_SOURCES modules/ffmpeg/vaapi_export.cpp)
    else()
        message(STATUS "未找到 libva：VAAPI 原生 DMA-BUF 导出禁用（硬解回退系统内存路径）")
    endif()
endif()

if(KOPAW_BUILD_OPENGL)
    list(APPEND KOPAW_MODULES_SOURCES modules/render/opengl/opengl_backend.cpp)
endif()

add_library(kopaw-modules STATIC ${KOPAW_MODULES_SOURCES})

target_include_directories(kopaw-modules PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
)
target_compile_definitions(kopaw-modules PUBLIC
    KOPAW_BUILD_VULKAN=$<BOOL:${KOPAW_BUILD_VULKAN}>
    KOPAW_BUILD_OPENGL=$<BOOL:${KOPAW_BUILD_OPENGL}>
    KOPAW_HAVE_LIBVA=${KOPAW_HAVE_LIBVA_VAL}
    # FFmpeg 头在 C++ 下要求
    __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
)

target_link_libraries(kopaw-modules PUBLIC
    kop-common
    PkgConfig::FFMPEG
)
if(KOPAW_BUILD_VULKAN)
    target_link_libraries(kopaw-modules PUBLIC Vulkan::Vulkan glfw)
    if(LIBVA_FOUND)
        target_link_libraries(kopaw-modules PUBLIC PkgConfig::LIBVA)
    endif()
endif()

if(KOPAW_BUILD_OPENGL)
    # GL 3.3 core：GL_GLEXT_PROTOTYPES 经 glvnd 直接链接，无加载器依赖
    target_link_libraries(kopaw-modules PUBLIC OpenGL::GL glfw)
endif()

# 着色器编译并嵌入（构建期，glslang 由 FetchContent 提供）
if(KOPAW_BUILD_VULKAN)
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules/render/shaders)
    set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GEN_DIR})
    add_custom_command(
        OUTPUT ${GEN_DIR}/shaders_embed.cpp
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=$<TARGET_FILE:glslang-standalone>
            -DSHADER_DIR=${SHADER_DIR}
            -DOUT_FILE=${GEN_DIR}/shaders_embed.cpp
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedShaders.cmake
        DEPENDS glslang-standalone ${SHADER_DIR}/quad.vert ${SHADER_DIR}/quad.frag
                ${SHADER_DIR}/nv12.frag
        VERBATIM
    )
    target_sources(kopaw-modules PRIVATE ${GEN_DIR}/shaders_embed.cpp)
endif()

# PortAudio（FetchContent 静态库；其 CMake 目标未传播 include 路径）
target_link_libraries(kopaw-modules PUBLIC portaudio_static)
target_include_directories(kopaw-modules PUBLIC ${portaudio_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# CLI 播放器
# ---------------------------------------------------------------------------
add_executable(kopaw-player tools/kopaw-player/src/main.cpp)
target_link_libraries(kopaw-player PRIVATE
    kopaw-modules
    kopaw_core          # Rust 图引擎（Corrosion 导入）
    pthread dl m
)
if(KOPAW_BUILD_KOPMS_SINK AND KOPAW_BUILD_VULKAN)
    target_link_libraries(kopaw-player PRIVATE kopaw-kopms-sink)
    target_compile_definitions(kopaw-player PRIVATE KOPAW_HAVE_KOPMS_SINK=1)
endif()
# P2.1：导出 kopaw_graph_* 符号供插件 .so 运行期解析
set_target_properties(kopaw-player PROPERTIES ENABLE_EXPORTS ON)

# ---------------------------------------------------------------------------
# P3-M3：KOPMS 零拷贝 sink（Vulkan DMA-BUF 导出 + BUS2LAYER 客户端）
# ---------------------------------------------------------------------------
option(KOPAW_BUILD_KOPMS_SINK "构建 KOPMS 零拷贝 sink 节点（需 Vulkan）" ON)
if(KOPAW_BUILD_KOPMS_SINK AND KOPAW_BUILD_VULKAN)
    add_library(kopaw-kopms-sink STATIC modules/kopms/kopms_sink_node.cpp)
    target_include_directories(kopaw-kopms-sink PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/modules
        ${CMAKE_SOURCE_DIR}/common/include
        ${CMAKE_SOURCE_DIR}/kopaw/abi/include)
    target_link_libraries(kopaw-kopms-sink PUBLIC kopaw-modules kopms-protocol)
endif()

# ---------------------------------------------------------------------------
# Demo 插件（.so 动态节点包；见 kopaw/abi/include/kopaw_plugin.h）
# ---------------------------------------------------------------------------
add_library(kopaw-plugin-demo MODULE plugins/demo/demo_plugin.cpp)
target_include_directories(kopaw-plugin-demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
)
target_link_libraries(kopaw-plugin-demo PRIVATE kop-common)
set_target_properties(kopaw-plugin-demo PROPERTIES OUTPUT_NAME "kopaw-plugin-demo"
                                                   PREFIX "lib" SUFFIX ".so")

# ---------------------------------------------------------------------------
# 封装/转码 CLI（P2.3）
# ---------------------------------------------------------------------------
add_executable(kopaw-transcode tools/kopaw-transcode/src/main.cpp)
target_include_directories(kopaw-transcode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
)
target_compile_definitions(kopaw-transcode PRIVATE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
target_link_libraries(kopaw-transcode PRIVATE kop-common PkgConfig::FFMPEG pthread)

# ---------------------------------------------------------------------------
# Focused ABI and lavfi tests. They run without a sound device or display.
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    if(KOPAW_BUILD_VULKAN)
        add_executable(kopaw-vk-ycbcr-test
            tests/vk_ycbcr_test.cpp
            modules/render/vulkan/vk_ycbcr.cpp)
        target_include_directories(kopaw-vk-ycbcr-test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/modules
            ${CMAKE_CURRENT_SOURCE_DIR}/abi/include
            ${Vulkan_INCLUDE_DIRS})
        add_test(NAME kopaw-vk-ycbcr COMMAND kopaw-vk-ycbcr-test)
    endif()

    add_executable(kopaw-filter-node-test tests/filter_node_test.cpp)
    target_link_libraries(kopaw-filter-node-test PRIVATE
        kopaw-modules
        kopaw_core
        pthread dl m
    )
    add_test(NAME kopaw-filter-node COMMAND kopaw-filter-node-test)

    add_library(kopaw-plugin-test-good MODULE tests/plugins/good_plugin.cpp)
    add_library(kopaw-plugin-test-bad-major MODULE tests/plugins/bad_major_plugin.cpp)
    add_library(kopaw-plugin-test-bad-size MODULE tests/plugins/bad_size_plugin.cpp)
    add_library(kopaw-plugin-test-bad-node MODULE tests/plugins/bad_node_plugin.cpp)
    add_library(kopaw-plugin-test-bad-binding MODULE tests/plugins/bad_binding_plugin.cpp)
    add_library(kopaw-plugin-test-missing-symbol MODULE tests/plugins/missing_symbol_plugin.cpp)
    foreach(target
            kopaw-plugin-test-good
            kopaw-plugin-test-bad-major
            kopaw-plugin-test-bad-size
            kopaw-plugin-test-bad-node
            kopaw-plugin-test-bad-binding
            kopaw-plugin-test-missing-symbol)
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/kopaw/abi/include)
        set_target_properties(${target} PROPERTIES PREFIX "lib" SUFFIX ".so")
    endforeach()

    add_executable(kopaw-plugin-loader-test tests/plugin_loader_test.cpp)
    set_target_properties(kopaw-plugin-loader-test PROPERTIES ENABLE_EXPORTS ON)
    target_link_libraries(kopaw-plugin-loader-test PRIVATE
        kopaw-modules
        kopaw_core
        pthread dl m
    )
    add_dependencies(kopaw-plugin-loader-test
        kopaw-plugin-test-good
        kopaw-plugin-test-bad-major
        kopaw-plugin-test-bad-size
        kopaw-plugin-test-bad-node
        kopaw-plugin-test-bad-binding
        kopaw-plugin-test-missing-symbol)
    add_test(NAME kopaw-plugin-loader
        COMMAND kopaw-plugin-loader-test
            $<TARGET_FILE:kopaw-plugin-test-good>
            $<TARGET_FILE:kopaw-plugin-test-bad-major>
            $<TARGET_FILE:kopaw-plugin-test-bad-size>
            $<TARGET_FILE:kopaw-plugin-test-bad-node>
            $<TARGET_FILE:kopaw-plugin-test-bad-binding>
            $<TARGET_FILE:kopaw-plugin-test-missing-symbol>)
endif()
