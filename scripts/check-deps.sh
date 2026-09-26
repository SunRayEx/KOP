#!/usr/bin/env bash
# KOP 依赖体检：检查构建所需系统库与工具是否就绪
set -u
fail=0

ok()   { printf "  \033[32m✔\033[0m %-22s %s\n" "$1" "$2"; }
bad()  { printf "  \033[31m✘\033[0m %-22s %s\n" "$1" "$2"; fail=1; }

echo "== 工具 =="
for t in cmake ninja pkg-config git; do
    command -v "$t" >/dev/null 2>&1 && ok "$t" "$($t --version 2>/dev/null | head -1)" \
        || bad "$t" "未安装"
done
command -v cargo >/dev/null 2>&1 && ok "cargo" "$(cargo --version)" || bad cargo 未安装
command -v cbindgen >/dev/null 2>&1 && ok cbindgen "$(cbindgen --version 2>/dev/null)" \
    || echo "  - cbindgen 未安装（仅重新生成 ABI 头时需要：cargo install cbindgen）"

echo "== FFmpeg (pkg-config) =="
for lib in libavformat libavcodec libavutil libswscale libswresample; do
    v=$(pkg-config --modversion "$lib" 2>/dev/null) && ok "$lib" "$v" || bad "$lib" 缺失
done

echo "== 图形 (pkg-config) =="
for lib in vulkan wayland-client xkbcommon egl; do
    v=$(pkg-config --modversion "$lib" 2>/dev/null) && ok "$lib" "$v" \
        || echo "  - $lib 缺失（仅影响对应后端）"
done

# GLFW（KOPMS 的 nested M1 输出）在 X11 后端下逐项检查这些扩展头文件，
# 缺任一个都在配置期直接 FATAL_ERROR。
echo "== GLFW 的 X11 依赖 =="
for lib in xrandr xinerama xcursor xi xext; do
    v=$(pkg-config --modversion "$lib" 2>/dev/null) && ok "$lib" "$v" \
        || bad "$lib" "GLFW 需要（对应 Debian: libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev）"
done

echo "== 音频 (pkg-config) =="
v=$(pkg-config --modversion alsa 2>/dev/null) && ok "alsa" "$v" || bad alsa 缺失

echo "== 网络 =="
ok "FetchContent 依赖" "首次配置需联网克隆 corrosion/glfw/portaudio/glslang"

exit $fail
