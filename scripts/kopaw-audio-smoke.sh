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
REQUIRE_MEDIA_COMPLETE=${KOPAW_AUDIO_REQUIRE_MEDIA_COMPLETE:-0}
REQUIRE_COUNTS=${KOPAW_AUDIO_REQUIRE_COUNTS:-1}
REQUIRE_CALLBACK=${KOPAW_AUDIO_REQUIRE_CALLBACK:-0}
REQUIRE_NO_XRUN=${KOPAW_AUDIO_REQUIRE_NO_XRUN:-0}
WATCHDOG_SEC=${KOPAW_AUDIO_WATCHDOG_SEC:-}
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
    media_duration=""
    if command -v ffprobe >/dev/null 2>&1; then
        probe=$(ffprobe -v error -select_streams a:0 \
            -show_entries stream=codec_name,sample_rate,channels,channel_layout \
            -of default=noprint_wrappers=1 "$media" 2>&1) || probe=""
        media_duration=$(ffprobe -v error -show_entries format=duration \
            -of default=noprint_wrappers=1:nokey=1 "$media" 2>/dev/null | head -1 || true)
        if [[ -z "$probe" && "$REQUIRE_AUDIO_STREAM" == 1 ]]; then
            echo "RESULT status=2 classification=no-audio-stream log=$log" >&2
            fail=1
            continue
        fi
        printf '%s\n' "$probe"
    fi
    if [[ -n "$WATCHDOG_SEC" ]]; then
        watchdog="$WATCHDOG_SEC"
    elif [[ "$DURATION" =~ ^[0-9]+([.][0-9]+)?$ && "$DURATION" != 0 ]]; then
        watchdog=$(awk -v d="$DURATION" 'BEGIN { printf "%.0f", d + 15.999 }')
    elif [[ "$media_duration" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        watchdog=$(awk -v d="$media_duration" 'BEGIN { printf "%.0f", d + 15.999 }')
    else
        watchdog=300
    fi
    if ! [[ "$watchdog" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "invalid watchdog timeout: $watchdog" >&2
        fail=1
        continue
    fi
    echo "duration_limit=$DURATION watchdog=${watchdog}s backend=$BACKEND"
    player_args=("$PLAYER" "$media" --no-video --duration "$DURATION" \
        --audio-backend "$BACKEND")
    if [[ -n "$DEVICE" ]]; then
        player_args+=(--audio-device "$DEVICE")
    fi
    set +e
    timeout --signal=TERM --kill-after=5 "${watchdog}s" \
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
    audio_stats=$(grep -aF '音频输出统计：' "$log" | tail -1 || true)
    callbacks=$(printf '%s\n' "$audio_stats" | sed -n 's/.*callbacks=\([0-9][0-9]*\).*/\1/p')
    frames=$(printf '%s\n' "$audio_stats" | sed -n 's/.*frames=\([0-9][0-9]*\).*/\1/p')
    consumed=$(printf '%s\n' "$audio_stats" | sed -n 's/.*consumed=\([0-9][0-9]*\).*/\1/p')
    callbacks=${callbacks:-unknown}
    frames=${frames:-unknown}
    consumed=${consumed:-unknown}
    underflow=$(printf '%s\n' "$audio_stats" | sed -n 's/.*underflow=\([0-9][0-9]*\).*/\1/p')
    overflow=$(printf '%s\n' "$audio_stats" | sed -n 's/.*overflow=\([0-9][0-9]*\).*/\1/p')
    underrun=$(printf '%s\n' "$audio_stats" | sed -n 's/.*underrun=\([0-9][0-9]*\).*/\1/p')
    underflow=${underflow:-unknown}
    overflow=${overflow:-unknown}
    underrun=${underrun:-unknown}
    state=$(printf '%s\n' "$stats" | sed -n 's/.*"state":"\([0-9][0-9]*\)".*/\1/p')
    state=${state:-unknown}
    sink_done=0
    { [[ "$state" == 3 ]] || grep -aFq '播放完成' "$log"; } && sink_done=1
    xrun=unknown
    if [[ "$underflow" =~ ^[0-9]+$ && "$overflow" =~ ^[0-9]+$ &&
          "$underrun" =~ ^[0-9]+$ ]]; then
        if [[ "$underflow" == 0 && "$overflow" == 0 && "$underrun" == 0 ]]; then
            xrun=0
        else
            xrun=1
        fi
    fi
    output_opened=0
    grep -aEq '音频输出设备:|音频输出后端: null' "$log" && output_opened=1
    played=0
    { grep -aFq 'state":"2"' "$log" || grep -aFq 'state":"3"' "$log" ||
      grep -aFq '播放完成' "$log"; } && played=1
    media_complete=0
    { grep -aFq 'state":"3"' "$log" || grep -aFq '播放完成' "$log"; } && media_complete=1

    if [[ "$status" == 124 || "$status" == 137 || "$status" == 143 ]]; then
        echo "RESULT status=$status classification=watchdog-timeout log=$log" >&2
        fail=1
    elif grep -aFq 'PortAudio 初始化失败' "$log"; then
        echo "RESULT status=$status classification=audio-backend-init-failed log=$log" >&2
        fail=1
    elif grep -aEq 'PortAudio 打开输出流失败|PortAudio 启动失败|默认设备不可用|找不到 PortAudio 输出设备|没有满足声道数要求的输出设备|没有 ALSA host API' "$log"; then
        echo "RESULT status=$status classification=audio-device-open-failed log=$log" >&2
        fail=1
    elif [[ "$status" != 0 ]]; then
        echo "RESULT status=$status classification=process-failure log=$log" >&2
        fail=1
    elif [[ "$REQUIRE_COUNTS" == 1 && ( ! "$decoder" =~ ^[0-9]+$ || ! "$sink" =~ ^[0-9]+$ ) ]]; then
        echo "RESULT status=$status classification=missing-audio-counts decoder=$decoder sink=$sink log=$log" >&2
        fail=1
    elif [[ "$output_opened" != 1 ]]; then
        echo "RESULT status=$status classification=no-audio-output log=$log" >&2
        fail=1
    elif [[ "$played" != 1 ]]; then
        echo "RESULT status=$status classification=audio-output-opened log=$log" >&2
        fail=1
    elif [[ "$REQUIRE_CALLBACK" == 1 && "$BACKEND" == null ]]; then
        echo "RESULT status=$status classification=no-audio-callback callbacks=0 backend=null log=$log" >&2
        fail=1
    elif [[ "$REQUIRE_CALLBACK" == 1 && "$BACKEND" != null &&
            ( ! "$callbacks" =~ ^[1-9][0-9]*$ ) ]]; then
        echo "RESULT status=$status classification=no-audio-callback callbacks=$callbacks log=$log" >&2
        fail=1
    elif [[ "$REQUIRE_NO_XRUN" == 1 && "$BACKEND" != null && "$xrun" != 0 ]]; then
        echo "RESULT status=$status classification=audio-xrun underflow=$underflow overflow=$overflow underrun=$underrun log=$log" >&2
        fail=1
    elif [[ "$REQUIRE_MEDIA_COMPLETE" == 1 && "$media_complete" != 1 ]]; then
        echo "RESULT status=$status classification=duration-limited log=$log" >&2
        fail=1
    elif [[ "$balanced" != 1 && "$REQUIRE_BALANCED" == 1 ]]; then
        echo "RESULT status=$status classification=audio-unbalanced decoder=$decoder sink=$sink log=$log" >&2
        fail=1
    elif [[ "$dropped" != 0 && "$REQUIRE_NO_DROP" == 1 ]]; then
        echo "RESULT status=$status classification=audio-dropped dropped=$dropped log=$log" >&2
        fail=1
    elif [[ "$dropped" != 0 ]]; then
        echo "RESULT status=$status classification=audio-playback-complete-with-drops dropped=$dropped callbacks=$callbacks frames=$frames consumed=$consumed underflow=$underflow overflow=$overflow underrun=$underrun media_complete=$media_complete sink_done=$sink_done log=$log"
    else
        echo "RESULT status=$status classification=audio-playback-complete dropped=0 balanced=$balanced callbacks=$callbacks frames=$frames consumed=$consumed underflow=$underflow overflow=$overflow underrun=$underrun media_complete=$media_complete sink_done=$sink_done log=$log"
    fi
    [[ -n "$stats" ]] || echo "warning: missing final statistics" >&2
    echo
done
exit "$fail"
