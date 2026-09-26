# KOP 模糊测试靶。
#
# 设计见 fuzz_driver.hpp：harness 只实现 LLVMFuzzerTestOneInput，由 KOP_BUILD_FUZZERS
# 决定运行方式：
#   - clang 可用 -fsanitize=fuzzer 时：真正的 libFuzzer 靶（无 main）。
#   - 否则（本仓库当前只用 GCC）：standalone 驱动 + ASan/UBSan，等价覆盖。
#
# 每个靶只直接编译被测解析器的源文件（这些模块自包含），因此整条链路都被
# sanitizer 插桩，不会漏报非插桩库里的越界。
#
# ctest 集成：每次跑一个有界的迭代预算（KOP_FUZZ_ITERS，默认见驱动），
# 作为 CI 冒烟；长时间“巡航”用 fuzz-long 目标。

# ---- 0) 编译器 ----------------------------------------------------------
# 编译器在顶层 CMakeLists.txt 的 project() 之前选定：CMake 在 project() 里
# 锁定编译器，之后的 check_cxx_compiler_flag/try_compile 只认已锁定那一个。
if(KOP_FUZZ_CXX)
    message(STATUS "fuzz: 模糊靶编译器 = ${KOP_FUZZ_CXX}")
endif()

# ---- 1) 探测 sanitizer 能力 --------------------------------------------------
include(CheckCXXCompilerFlag)
set(KOP_FUZZ_SAN_FLAGS "")
set(KOP_FUZZ_LINK_FLAGS "")
set(KOP_FUZZ_STANDALONE_DEFINE "KOP_FUZZ_STANDALONE")
set(KOP_FUZZ_ENGINE "none")

check_cxx_compiler_flag("-fsanitize=address" KOP_FUZZ_HAS_ASAN)
check_cxx_compiler_flag("-fsanitize=undefined" KOP_FUZZ_HAS_UBSAN)

# check_cxx_compiler_flag 只做编译（不链接），对 -fsanitize=fuzzer 不可靠：
# 它需要链接 libclang_rt.fuzzer 才能判定。用真正的链接测试代替。
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
    "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long){return 0;}
     /* libFuzzer 提供 main */"
    KOP_FUZZ_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

if(KOP_FUZZ_HAS_LIBFUZZER AND NOT KOP_FUZZ_FORCE_STANDALONE)
    message(STATUS "fuzz: 使用 libFuzzer（clang）")
    set(KOP_FUZZ_ENGINE "libfuzzer")
    set(KOP_FUZZ_SAN_FLAGS "-fsanitize=fuzzer-no-link,address,undefined")
    set(KOP_FUZZ_LINK_FLAGS "-fsanitize=fuzzer,address,undefined")
    set(KOP_FUZZ_STANDALONE_DEFINE "")  # libFuzzer 提供 main
elseif(KOP_FUZZ_HAS_UBSAN)
    # 只有 UBSan 可用时也不阻断构建：解析器类 bug（整数溢出、类型混淆、
    # 越界偏移计算）大多能被 UBSan 抓到，内存错误则留给 standalone 驱动的
    # 信号处理兜底。ASan 缺运行时时单独退化，不一票否决。
    message(STATUS "fuzz: standalone 驱动 + UBSan（ASan 运行时不可用）")
    set(KOP_FUZZ_ENGINE "standalone")
    set(KOP_FUZZ_SAN_FLAGS "-fsanitize=undefined;-fno-omit-frame-pointer")
    set(KOP_FUZZ_LINK_FLAGS "-fsanitize=undefined")
else()
    message(FATAL_ERROR "KOP_BUILD_FUZZERS 需要 address+undefined sanitizer 支持（当前编译器均不可用）")
endif()

add_library(kop-fuzz-driver STATIC fuzz_driver.cpp)
target_include_directories(kop-fuzz-driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(kop-fuzz-driver PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_options(kop-fuzz-driver PRIVATE ${KOP_FUZZ_SAN_FLAGS})
target_link_options(kop-fuzz-driver PRIVATE ${KOP_FUZZ_LINK_FLAGS})

# ---- 2) 靶定义：harness + 被测源 + 驱动 ---------------------------------------
# 每项三元组 = (靶名, harness 源, 被测源)；被测源所在目录自动加入 include 路径。

# 平行列表：靶名 / harness 源 / 被测源（同下标对应）。被测源所在目录自动
# 加入 include 路径，因此这些自包含的协议模块无需依赖完整 kopnet 构建产物。
set(KOP_FUZZ_NAMES
    kopnet-rtp-fuzz
    kopnet-tunnel-fuzz
    kopnet-frame-envelope-fuzz)
set(KOP_FUZZ_HARNESS
    fuzz_rtp.cpp
    fuzz_tunnel.cpp
    fuzz_frame_envelope.cpp)
set(KOP_FUZZ_UNDER_TEST
    ${CMAKE_SOURCE_DIR}/kopnet/protocol/rtp_framing.cpp
    ${CMAKE_SOURCE_DIR}/kopnet/tunnel/tunnel_protocol.cpp
    ${CMAKE_SOURCE_DIR}/kopnet/nodes/frame_envelope.cpp)

foreach(name IN LISTS KOP_FUZZ_NAMES)
    list(FIND KOP_FUZZ_NAMES "${name}" idx)
    list(GET KOP_FUZZ_HARNESS ${idx} harness)
    list(GET KOP_FUZZ_UNDER_TEST ${idx} under_test)
    get_filename_component(under_test_dir "${under_test}" DIRECTORY)

    # standalone 模式下 main() 在 fuzz_driver.cpp 里，必须编进可执行文件；
    # libFuzzer 模式由其库提供 main，不需要（且不能重复定义）。
    add_executable(${name} ${harness} ${under_test}
        $<$<BOOL:${KOP_FUZZ_STANDALONE_DEFINE}>:${CMAKE_CURRENT_SOURCE_DIR}/fuzz_driver.cpp>)
    if(KOP_FUZZ_STANDALONE_DEFINE)
        target_compile_definitions(${name} PRIVATE ${KOP_FUZZ_STANDALONE_DEFINE})
    endif()
    target_compile_options(${name} PRIVATE ${KOP_FUZZ_SAN_FLAGS})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/common/include
        ${CMAKE_SOURCE_DIR}/kopaw/abi/include
        ${CMAKE_SOURCE_DIR}/kopaw/modules
        ${under_test_dir}
        ${CMAKE_SOURCE_DIR}/kopnet/include)
    target_link_libraries(${name} PRIVATE kop-fuzz-driver kop-common)

    # ctest 冒烟：有界迭代；失败即 CI 拦截。
    add_test(NAME ${name} COMMAND ${name}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "KOP_FUZZ_ITERS=20000;ASAN_OPTIONS=detect_leaks=0")
    target_link_options(${name} PRIVATE ${KOP_FUZZ_LINK_FLAGS})
endforeach()

# ---- 3) 长时间巡航目标（本地深挖，不进 ctest）----
# cmake --build ... --target fuzz-long
add_custom_target(fuzz-long
    COMMAND ${CMAKE_CTEST_COMMAND} -R "fuzz" --output-on-failure
    COMMENT "运行全部模糊测试靶（短预算）")
