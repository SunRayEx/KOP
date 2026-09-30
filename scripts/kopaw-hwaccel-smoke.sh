#!/usr/bin/env bash
# KOPAW 真机硬解/回退 smoke：比较 none/vaapi/cuda/auto 的解码与呈现路径。
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

classify() {
    local log=$1 status=$2 label
    if grep -q '硬解实际生效' "$log"; then
        label=hardware-frame
    elif grep -q '硬解后端已初始化' "$log"; then
        label=backend-initialized-no-hardware-frame
    elif grep -q '视频软解已启用' "$log"; then
        label=software
    else
        label=unknown
    fi
    grep -q 'VAAPI DMA-BUF 导出器就绪\|解码表面原生 DMA-BUF 导出' "$log" && label+='+dmabuf-export' || true
    grep -q 'DMA-BUF 导入失败' "$log" && label+='+import-fallback' || true
    grep -q '导入 YUV' "$log" && label+='+dmabuf-import' || true
    grep -q '绘制失败，终止渲染\|窗口关闭，停止播放' "$log" && label+='+render-failure' || true
    [[ "$status" == 124 || "$status" == 143 ]] && label+='+timeout' || true
    printf '%s\n' "$label"
}

fail=0
for mode in none vaapi cuda auto; do
    log="$LOG_DIR/$mode.log"
    echo "== KOPAW_HWACCEL=$mode =="
    set +e
    timeout --signal=TERM --kill-after=5 "$((DURATION + 15))" \
        env KOPAW_HWACCEL="$mode" \
        KOPAW_VK_DEVICE="${KOPAW_VK_DEVICE:-}" \
        "$PLAYER" "$MEDIA" --duration "$DURATION" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    result=$(classify "$log" "$status")
    echo "RESULT mode=$mode status=$status classification=$result log=$log"
    if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then
        echo "mode $mode failed with status $status" >&2
        fail=1
    fi
    if [[ "$result" == unknown* || "$result" == *render-failure* ]]; then
        echo "mode $mode did not reach a classifiable successful render path" >&2
        fail=1
    fi
done

exit "$fail"
