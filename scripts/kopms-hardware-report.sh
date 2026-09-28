#!/usr/bin/env bash
# KOPMS 真机能力报告：只读，不取得 DRM master，不执行 modeset。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
COMPOSITOR="$BUILD_DIR/kopms/kopms-compositor"

printf '== session ==\n'
printf 'XDG_SESSION_TYPE=%s\n' "${XDG_SESSION_TYPE:-}"
printf 'WAYLAND_DISPLAY=%s\n' "${WAYLAND_DISPLAY:-}"
printf 'DISPLAY=%s\n' "${DISPLAY:-}"
printf 'XDG_RUNTIME_DIR=%s\n' "${XDG_RUNTIME_DIR:-}"

printf '\n== DRM nodes ==\n'
shopt -s nullglob
nodes=(/dev/dri/card*)
if ((${#nodes[@]} == 0)); then
    echo 'no /dev/dri/card* found'
else
    for node in "${nodes[@]}"; do
        echo "--- $node"
        if command -v udevadm >/dev/null 2>&1; then
            udevadm info -q property -n "$node" 2>/dev/null |
                grep -E '^(DEVNAME|DRIVER|ID_PATH|ID_SEAT)=' || true
        fi
        if [[ -x "$COMPOSITOR" ]]; then
            timeout "${KOPMS_PROBE_TIMEOUT:-15}" "$COMPOSITOR" \
                --probe-drm --drm-device "$node" 2>&1 |
                grep -E 'DRM/KMS|输出候选|driver=|connector=|mode=' || true
        fi
    done
fi

printf '\n== GPU ==\n'
command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L || true
command -v vulkaninfo >/dev/null 2>&1 && \
    vulkaninfo --summary 2>/dev/null | grep -E 'GPU|deviceName|driverName' | head -30 || true

printf '\n== kernel DRM modules ==\n'
if command -v lsmod >/dev/null 2>&1; then
    lsmod | grep -E '^(i915|xe|amdgpu|nouveau|nvidia_drm|vkms|drm)' || true
fi
