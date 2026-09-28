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
