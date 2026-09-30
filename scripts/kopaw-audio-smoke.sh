#!/usr/bin/env bash
# KOPAW 音频 smoke：验证解复用、解码、系统默认音频输出和收尾统计。
# 不要求 DISPLAY/WAYLAND_DISPLAY；播放器使用 --no-video。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
PLAYER="$BUILD_DIR/kopaw/kopaw-player"
DURATION=${KOPAW_AUDIO_DURATION:-5}
LOG_DIR=${KOPAW_AUDIO_LOG_DIR:-$ROOT/build/kopaw-audio-smoke}
mkdir -p "$LOG_DIR"

[[ -x "$PLAYER" ]] || { echo "missing $PLAYER" >&2; exit 2; }
if (($# == 0)); then
    echo "usage: $0 audio-file [...]" >&2
    exit 2
fi

fail=0
index=0
for media in "$@"; do
    [[ -f "$media" ]] || { echo "missing media: $media" >&2; fail=1; continue; }
    index=$((index + 1))
    log="$LOG_DIR/${index}-$(basename "$media").log"
    echo "== audio: $media =="
    if command -v ffprobe >/dev/null 2>&1; then
        ffprobe -v error -select_streams a:0 \
            -show_entries stream=codec_name,sample_rate,channels,channel_layout \
            -of default=noprint_wrappers=1 "$media" || true
    fi
    set +e
    timeout --signal=TERM --kill-after=5 "$((DURATION + 15))" \
        env KOPAW_HWACCEL=none \
        "$PLAYER" "$media" --no-video --duration "$DURATION" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    stats=$(grep -F '最终统计:' "$log" | tail -1 || true)
    if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then
        echo "RESULT status=$status classification=process-failure log=$log" >&2
        fail=1
    elif grep -q '音频输出设备:' "$log" && grep -q 'state":"2"' "$log"; then
        echo "RESULT status=$status classification=audio-playback-complete log=$log"
    elif grep -q '音频输出设备:' "$log"; then
        echo "RESULT status=$status classification=audio-output-opened log=$log"
        fail=1
    else
        echo "RESULT status=$status classification=no-audio-output log=$log" >&2
        fail=1
    fi
    [[ -n "$stats" ]] || echo "warning: missing final statistics" >&2
    echo
 done
exit "$fail"
