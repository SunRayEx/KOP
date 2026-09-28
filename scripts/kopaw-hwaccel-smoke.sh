#!/usr/bin/env bash
# KOPAW 真机硬解/回退 smoke：比较 none/vaapi/cuda/auto 的启动与输出日志。
# 需要输入媒体；不会修改系统 GPU 状态。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
PLAYER="$BUILD_DIR/kopaw/kopaw-player"
MEDIA=${1:-${KOPAW_MEDIA:-}}
DURATION=${KOPAW_DURATION:-5}
LOG_DIR=${KOPAW_LOG_DIR:-$ROOT/build/kopaw-hw-smoke}
mkdir -p "$LOG_DIR"

[[ -x "$PLAYER" ]] || { echo "missing $PLAYER" >&2; exit 2; }
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" &&
      "${KOPAW_ALLOW_HEADLESS:-0}" != 1 ]]; then
    echo "KOPAW smoke skipped: no DISPLAY/WAYLAND_DISPLAY (TTY/headless)"
    exit 77
fi
[[ -n "$MEDIA" && -f "$MEDIA" ]] || {
    echo "usage: $0 input.mkv" >&2
    exit 2
}

fail=0
for mode in none vaapi cuda auto; do
    log="$LOG_DIR/$mode.log"
    echo "== KOPAW_HWACCEL=$mode =="
    set +e
    timeout --signal=TERM --kill-after=5 "$((DURATION + 15))" \
        env KOPAW_HWACCEL="$mode" \
        "$PLAYER" "$MEDIA" --duration "$DURATION" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then
        echo "mode $mode failed with status $status" >&2
        fail=1
    fi
    if ! grep -Eqi '硬解|hwaccel|CUDA|VAAPI|回退|播放|完成|frame' "$log"; then
        echo "warning: no recognizable decoder marker for $mode" >&2
    fi
done

exit "$fail"
