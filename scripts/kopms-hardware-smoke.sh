#!/usr/bin/env bash
# KOPMS 真机 smoke：运行 compositor + test client，默认使用 nested 输出。
# 不会自动接管真实 DRM；需要直出时显式传 --direct-drm。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
SOCKET=${KOPMS_SOCKET:-kop-smoke-$$}
SECONDS_RUN=${KOPMS_SECONDS:-8}
LOG_DIR=${KOPMS_LOG_DIR:-$ROOT/build/kopms-smoke}
mkdir -p "$LOG_DIR"
COMPOSITOR="$BUILD_DIR/kopms/kopms-compositor"

# 不信任 TTY/root 继承的 /run/user/0；nested Wayland 也使用独立 runtime。
RUNTIME_DIR=${KOPMS_RUNTIME_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/kopms-runtime.XXXXXX")}
RUNTIME_OWNED=0
[[ -n "${KOPMS_RUNTIME_DIR:-}" ]] || RUNTIME_OWNED=1
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"
trap 'if [[ "$RUNTIME_OWNED" == 1 ]]; then rm -rf "$RUNTIME_DIR"; fi' EXIT INT TERM
CLIENT="$BUILD_DIR/kopms/kopms-test-client"

[[ -x "$COMPOSITOR" ]] || { echo "missing $COMPOSITOR" >&2; exit 2; }
[[ -x "$CLIENT" ]] || { echo "missing $CLIENT" >&2; exit 2; }
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" &&
      "${KOPMS_ALLOW_HEADLESS:-0}" != 1 ]]; then
    echo "nested smoke skipped: no DISPLAY/WAYLAND_DISPLAY (TTY/headless)"
    exit 77
fi

args=("$SOCKET" "$SECONDS_RUN")
if [[ -n "${KOPMS_DRM_DEVICE:-}" ]]; then args+=(--drm-device "$KOPMS_DRM_DEVICE"); fi
if [[ "${KOPMS_DIRECT_DRM:-0}" == 1 ]]; then args+=(--direct-drm); fi
if [[ "${KOPMS_ALLOW_UNMANAGED_DRM:-0}" == 1 ]]; then args+=(--allow-unmanaged-drm); fi
if [[ "${KOPMS_SCENE_WINDOW:-0}" == 1 ]]; then args+=(--scene-window); fi
[[ -n "${KOPMS_RESOLUTION:-}" ]] && args+=(--resolution "$KOPMS_RESOLUTION")
[[ -n "${KOPMS_SCALE:-}" ]] && args+=(--scale "$KOPMS_SCALE")
[[ -n "${KOPMS_COLORSPACE:-}" ]] && args+=(--colorspace "$KOPMS_COLORSPACE")
[[ -n "${KOPMS_HDR:-}" ]] && args+=(--hdr "$KOPMS_HDR")

cleanup() { [[ -n "${pid:-}" ]] && kill "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
unset WAYLAND_DISPLAY WAYLAND_SOCKET
"$COMPOSITOR" "${args[@]}" >"$LOG_DIR/compositor.log" 2>&1 &
pid=$!
for _ in $(seq 1 100); do
    if grep -q 'KOPMS 运行中' "$LOG_DIR/compositor.log" 2>/dev/null; then break; fi
    kill -0 "$pid" 2>/dev/null || { cat "$LOG_DIR/compositor.log"; exit 1; }
    sleep 0.1
done

if ! grep -q 'KOPMS 运行中' "$LOG_DIR/compositor.log"; then
    cat "$LOG_DIR/compositor.log"
    echo 'compositor did not become ready' >&2
    exit 1
fi

WAYLAND_DISPLAY="$SOCKET" timeout "$((SECONDS_RUN + 5))" "$CLIENT" \
    "$((SECONDS_RUN > 2 ? SECONDS_RUN - 2 : 1))" >"$LOG_DIR/client.log" 2>&1 || {
        cat "$LOG_DIR/client.log"
        exit 1
    }
wait "$pid" || true
cat "$LOG_DIR/compositor.log"
cat "$LOG_DIR/client.log"
echo "KOPMS smoke passed; logs: $LOG_DIR"
