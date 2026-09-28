#!/usr/bin/env bash
# KOPMS DRM 真机 smoke。危险：会通过 logind 接管显示设备并执行 modeset。
# 只在独立 tty/kiosk/vkms 环境运行。默认不使用 --allow-unmanaged-drm。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
DEVICE=${KOPMS_DRM_DEVICE:-/dev/dri/card1}
SOCKET=${KOPMS_SOCKET:-kop-drm-smoke-$$}
SECONDS_RUN=${KOPMS_SECONDS:-10}
LOG_DIR=${KOPMS_LOG_DIR:-$ROOT/build/kopms-drm-smoke}
mkdir -p "$LOG_DIR"
COMPOSITOR="$BUILD_DIR/kopms/kopms-compositor"

# Wayland 的 socket lockfile 位于 XDG_RUNTIME_DIR。TTY/root 环境经常没有
# 可写的 /run/user/0，或残留了桌面用户创建的同名 lock；使用一次性私有目录
# 避免把 DRM 测试和桌面 runtime 混在一起。
RUNTIME_DIR=${KOPMS_RUNTIME_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/kopms-drm-runtime.XXXXXX")}
RUNTIME_OWNED=0
if [[ -z "${KOPMS_RUNTIME_DIR:-}" ]]; then RUNTIME_OWNED=1; fi
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"
# 同一 socket 名可能被上一次异常退出留下；只清理本次私有 runtime 中的文件。
rm -f "$RUNTIME_DIR/$SOCKET" "$RUNTIME_DIR/$SOCKET.lock" \
      "$RUNTIME_DIR/$SOCKET.bus" "$RUNTIME_DIR/$SOCKET.bus.lock"
[[ -d "$RUNTIME_DIR" && -w "$RUNTIME_DIR" ]] || {
    echo "XDG_RUNTIME_DIR is not writable: $RUNTIME_DIR" >&2
    exit 2
}
cleanup_runtime() {
    if [[ "$RUNTIME_OWNED" == 1 ]]; then rm -rf "$RUNTIME_DIR"; fi
}
trap cleanup_runtime EXIT INT TERM

[[ -x "$COMPOSITOR" ]] || { echo "missing $COMPOSITOR" >&2; exit 2; }
[[ -e "$DEVICE" ]] || { echo "missing DRM device $DEVICE" >&2; exit 2; }

if [[ "${KOPMS_CONFIRM_DRM:-}" != "YES" ]]; then
    cat >&2 <<EOF
This test will acquire $DEVICE and perform a real DRM modeset/page-flip.
Run it only from an isolated tty/kiosk/vkms environment.
Set KOPMS_CONFIRM_DRM=YES to continue.
EOF
    exit 2
fi

args=("$SOCKET" "$SECONDS_RUN" --direct-drm --drm-device "$DEVICE" --watch-drm)
[[ "${KOPMS_ALLOW_UNMANAGED_DRM:-0}" == 1 ]] && args+=(--allow-unmanaged-drm)
[[ -n "${KOPMS_RESOLUTION:-}" ]] && args+=(--resolution "$KOPMS_RESOLUTION")
[[ -n "${KOPMS_SCALE:-}" ]] && args+=(--scale "$KOPMS_SCALE")
[[ -n "${KOPMS_COLORSPACE:-}" ]] && args+=(--colorspace "$KOPMS_COLORSPACE")
[[ -n "${KOPMS_HDR:-}" ]] && args+=(--hdr "$KOPMS_HDR")

echo "DRM device=$DEVICE socket=$SOCKET runtime=$RUNTIME_DIR"
# export 在 timeout/env 的两层进程中都明确生效，避免 TTY 的 /run/user/0 泄漏。
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
unset WAYLAND_DISPLAY WAYLAND_SOCKET
set +e
timeout --signal=TERM --kill-after=5 "$((SECONDS_RUN + 8))" \
    "$COMPOSITOR" "${args[@]}" >"$LOG_DIR/compositor.log" 2>&1
status=$?
set -e
cat "$LOG_DIR/compositor.log"

if grep -qE 'page-flip|atomic modeset active|DRM 直出|DRM 输出' "$LOG_DIR/compositor.log"; then
    echo "DRM direct smoke completed; inspect $LOG_DIR/compositor.log"
else
    echo "DRM direct smoke did not show expected KMS markers" >&2
    exit 1
fi
# timeout(124/143) is expected when the compositor is intentionally time-bounded.
[[ "$status" == 0 || "$status" == 124 || "$status" == 143 ]]
