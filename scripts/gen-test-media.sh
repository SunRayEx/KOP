#!/usr/bin/env bash
# 生成 KOPAW MVP 测试媒体：testsrc2 视频图卡 + 440Hz 音调，默认 30s 1280x720@30。
# 优先使用 libx264（若可用），否则退回 mpeg4；音频 mp2（Fedora 免费构建自带）。
set -euo pipefail

OUT="${1:-test_media.mkv}"
DUR="${2:-30}"
SIZE="${3:-1280x720}"
RATE="${4:-30}"

VCODEC=mpeg4
EXTRA_V=""
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q " libx264 "; then
    VCODEC=libx264
    EXTRA_V="-preset ultrafast -crf 28"
fi

echo "生成 $OUT（${DUR}s ${SIZE}@${RATE}fps，编码 ${VCODEC} + mp2）"
ffmpeg -y -hide_banner -loglevel warning \
    -f lavfi -i "testsrc2=size=${SIZE}:rate=${RATE}" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -t "$DUR" \
    -c:v "$VCODEC" $EXTRA_V \
    -c:a mp2 -b:a 192k \
    -pix_fmt yuv420p \
    "$OUT"
echo "完成: $OUT"
