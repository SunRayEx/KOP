#!/usr/bin/env bash
# 本地复现 CI 的模糊测试冒烟：构建并运行全部 fuzz 靶（短预算）。
#
# 用法：
#   scripts/run-fuzzers.sh                # 默认 200000 次运行
#   scripts/run-fuzzers.sh 1000000        # 指定次数
#   KOP_FUZZ_ENGINE=standalone scripts/run-fuzzers.sh   # 用 standalone 驱动而非 libFuzzer
#
# libFuzzer 模式需要 clang；没有 clang 时用 KOP_FUZZ_ENGINE=standalone，
# standalone 驱动不依赖 libFuzzer 运行时。
set -euo pipefail

runs="${1:-200000}"
engine="${KOP_FUZZ_ENGINE:-libfuzzer}"
build_dir="${KOP_FUZZ_BUILD_DIR:-build/fuzz}"

cd "$(dirname "$0")/.."

extra=()
if [ "$engine" = "standalone" ]; then
    extra+=( -DKOP_FUZZ_FORCE_STANDALONE=ON )
fi

# 离线复用：若已有其它构建目录下载过 FetchContent 源码，直接指过去，
# 避免无网络环境下卡在 git clone。优先认 relwithdebinfo，其次 build。
reuse_args=()
for d in build/relwithdebinfo build; do
    if [ -d "$d/_deps" ]; then
        for name in corrosion glfw portaudio glslang; do
            if [ -d "$d/_deps/$name-src" ]; then
                reuse_args+=("-DFETCHCONTENT_SOURCE_DIR_${name^^}=$PWD/$d/_deps/$name-src")
            fi
        done
        break
    fi
done
if [ "${#reuse_args[@]}" -gt 0 ]; then
    echo "    复用已下载依赖：${#reuse_args[@]} 项"
fi

echo "==> 配置（引擎=$engine，构建目录=$build_dir）"
# corrosion 通过 rustup 查找工具链；用非默认工具链（如 nightly）时必须
# 显式声明，否则 rustup show 会报 Missing manifest in toolchain 'stable'。
export RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-nightly}"
cmake -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DKOP_BUILD_FUZZERS=ON \
    -DKOP_FUZZ_CXX="${KOP_FUZZ_CXX:-clang++}" \
    -DKOPAW_BUILD_VULKAN=OFF -DKOPAW_BUILD_OPENGL=OFF \
    -DKOPNET_BUILD_NODES=OFF \
    "${extra[@]}" \
    "${reuse_args[@]}"

targets=(kopnet-rtp-fuzz kopnet-tunnel-fuzz kopnet-frame-envelope-fuzz)
echo "==> 构建：${targets[*]}"
cmake --build "$build_dir" --target "${targets[@]}"

fail=0
for t in "${targets[@]}"; do
    bin="$build_dir/fuzz/$t"
    echo "==> 运行 $t（$runs 次）"
    if [ "$engine" = "standalone" ]; then
        # standalone 驱动用环境变量控制迭代数，崩溃输入落在 cwd
        # 用子 shell 切到构建目录：崩溃输入（crash-*.bin）落在那里
        ( KOP_FUZZ_ITERS="$runs" ASAN_OPTIONS=detect_leaks=0 \
              cd "$build_dir/fuzz" && ./"$t" ) || fail=1
    else
        "$bin" -runs="$runs" -max_len=4096 -rss_limit_mb=2048 -timeout=30 || fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "✘ 模糊测试发现崩溃" >&2
    exit 1
fi
echo "✔ 全部 fuzz 靶通过（$runs 次/靶）"
