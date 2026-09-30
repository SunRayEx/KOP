#!/usr/bin/env bash
# KOPAW 音频 smoke：验证解复用、解码、系统默认音频输出和收尾统计。
# 不要求 DISPLAY/WAYLAND_DISPLAY；播放器使用 --no-video。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${KOP_BUILD_DIR:-$ROOT/build/relwithdebinfo}
PLAYER="$BUILD_DIR/kopaw/kopaw-player"
DURATION=${KOPAW_AUDIO_DURATION:-5}
BACKEND=${KOPAW_AUDIO_BACKEND:-auto}
DEVICE=${KOPAW_AUDIO_DEVICE:-}
LIST_DEVICES=${KOPAW_AUDIO_LIST_DEVICES:-0}
INFO_ONLY=${KOPAW_AUDIO_INFO_ONLY:-0}
REQUIRE_NO_DROP=${KOPAW_AUDIO_REQUIRE_NO_DROP:-0}
REQUIRE_BALANCED=${KOPAW_AUDIO_REQUIRE_BALANCED:-0}
REQUIRE_AUDIO_STREAM=${KOPAW_AUDIO_REQUIRE_STREAM:-1}
LOG_DIR=${KOPAW_AUDIO_LOG_DIR:-$ROOT/build/kopaw-audio-smoke}
mkdir -p "$LOG_DIR"

[[ -x "$PLAYER" ]] || { echo "missing $PLAYER" >&2; exit 2; }
if [[ "$LIST_DEVICES" == 1 ]]; then
    exec "$PLAYER" --list-audio-devices
fi
if [[ "$INFO_ONLY" == 1 ]]; then
    exec "$PLAYER" --audio-info
fi
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
    if [[ "$BACKEND" != auto && "$BACKEND" != alsa && "$BACKEND" != null ]]; then
        echo "invalid KOPAW_AUDIO_BACKEND: $BACKEND" >&2
        fail=1
        continue
    fi
    if command -v ffprobe >/dev/null 2>&1; then
        probe=$(ffprobe -v error -select_streams a:0 \
            -show_entries stream=codec_name,sample_rate,channels,channel_layout \
            -of default=noprint_wrappers=1 "$media" 2>&1) || probe=""
        if [[ -z "$probe" && "$REQUIRE_AUDIO_STREAM" == 1 ]]; then
            echo "RESULT status=2 classification=no-audio-stream log=$log" >&2
            fail=1
            continue
        fi
        printf '%s\n' "$probe"
    fi
    player_args=("$PLAYER" "$media" --no-video --duration "$DURATION" \
        --audio-backend "$BACKEND")
    if [[ -n "$DEVICE" ]]; then
        player_args+=(--audio-device "$DEVICE")
    fi
    set +e
    timeout --signal=TERM --kill-after=5 "$((DURATION + 15))" \
        env KOPAW_HWACCEL=none "${player_args[@]}" >"$log" 2>&1
    status=$?
    set -e
    cat "$log"
    stats=$(grep -aF '最终统计:' "$log" | tail -1 || true)
    dropped=$(printf '%s\n' "$stats" | sed -n 's/.*"dropped":\([0-9][0-9]*\).*/\1/p')
    dropped=${dropped:-unknown}
    decoder=$(printf '%s\n' "$stats" | sed -n 's/.*"name":"audio_decoder","delivered":\([0-9][0-9]*\).*/\1/p')
    sink=$(printf '%s\n' "$stats" | sed -n 's/.*"name":"audio_sink","delivered":\([0-9][0-9]*\).*/\1/p')
    balanced=unknown
    if [[ "$decoder" =~ ^[0-9]+$ && "$sink" =~ ^[0-9]+$ ]]; then
        [[ "$decoder" == "$sink" ]] && balanced=1 || balanced=0
    fi
    if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then
        echo "RESULT status=$status classification=process-failure log=$log" >&2
        fail=1
    elif grep -aEq '音频输出设备:|音频输出后端: null' "$log" &&
         { grep -aFq 'state":"2"' "$log" || grep -aFq '播放完成' "$log"; }; then
        if [[ "$balanced" != 1 && "$REQUIRE_BALANCED" == 1 ]]; then
            echo "RESULT status=$status classification=audio-unbalanced decoder=$decoder sink=$sink log=$log" >&2
            fail=1
        elif [[ "$dropped" != 0 && "$REQUIRE_NO_DROP" == 1 ]]; then
            echo "RESULT status=$status classification=audio-dropped dropped=$dropped log=$log" >&2
            fail=1
        elif [[ "$dropped" != 0 ]]; then
            echo "RESULT status=$status classification=audio-playback-complete-with-drops dropped=$dropped log=$log"
        else
            echo "RESULT status=$status classification=audio-playback-complete dropped=0 balanced=$balanced log=$log"
        fi
    elif grep -aEq '音频输出设备:|音频输出后端: null' "$log"; then
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
