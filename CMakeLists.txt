cmake_minimum_required(VERSION 3.24)

# CMake 4.x 兼容：允许较老第三方项目（portaudio/glslang）的最低版本声明
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(kop VERSION 0.1.0 LANGUAGES C CXX
    DESCRIPTION "KOPAW media pipeline + KOPMS Wayland compositor"
    HOMEPAGE_URL "https://example.invalid/kop")
set(CPACK_PACKAGE_LICENSE "GPL-3.0-or-later")

include(CTest)
install(FILES LICENSE DESTINATION .)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(KOPAW_BUILD_VULKAN "构建 Vulkan 渲染后端（首选）" ON)
option(KOPAW_BUILD_OPENGL "构建 OpenGL 渲染后端（GL 3.3 core）" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# 第三方依赖（系统缺失者一律源码构建，项目自包含）
# ---------------------------------------------------------------------------
# Rust 集成
FetchContent_Declare(corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion
    GIT_TAG v0.5.2
)
FetchContent_MakeAvailable(corrosion)

# 窗口与输入（KOPAW 的 Vulkan/GL surface，以及 KOPMS nested 输出）
# KOPMS 当前固定使用 GLFW + OpenGL 的 nested M1 输出，因此即使两个
# KOPAW 渲染后端都关闭，也必须保留这项依赖。
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw
    GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)

if(KOPAW_BUILD_OPENGL)
    find_package(OpenGL REQUIRED)
endif()

# 音频后端（ALSA 路径，经 PipeWire 的 ALSA 兼容层出声）
set(PA_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PA_USE_JACK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(portaudio
    GIT_REPOSITORY https://github.com/PortAudio/portaudio
    GIT_TAG v19.7.0
)
FetchContent_MakeAvailable(portaudio)

# 着色器编译器（构建期工具，GLSL → SPIR-V → 嵌入二进制）
if(KOPAW_BUILD_VULKAN)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang
        GIT_TAG 15.4.0
        GIT_SHALLOW TRUE  # 标签固定：浅克隆体积小得多，弱网环境重试成功率更高
    )
    FetchContent_MakeAvailable(glslang)
    find_package(Vulkan REQUIRED)
endif()

# ---------------------------------------------------------------------------
# 项目目标
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample libavfilter)

add_subdirectory(common)
add_subdirectory(kopaw)

add_subdirectory(kopms)

# KOP_AppSDK_Interface + 用户级测试应用（KOPAW_Test / KOPMS_Test）
add_subdirectory(sdk)
add_subdirectory(apps)
