#!/bin/sh
# End-to-end check that replay-transcode's NUT output is a valid container that
# ffmpeg demuxes: build a synthetic movie (video + signed-16 stereo sound),
# transcode it both ways, and confirm ffmpeg decodes the NUT stream back to the
# exact same RGB24 video as raw mode and the exact same PCM as the source.
#
# ffmpeg is the correctness oracle here -- the muxer's framing/CRC/varint coding
# is also checked without ffmpeg by test_nut_mux. Skips (77) if ffmpeg is absent.
set -eu

maker=$1
transcode=$2
ffmpeg=$3

if ! command -v "$ffmpeg" >/dev/null 2>&1; then
    echo "ffmpeg not available; skipping"
    exit 77
fi

work=${TMPDIR:-/tmp}/transcode-nut-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

"$maker" "$work/m.ae7" "$work/expected.pcm"

# Raw mode: RGB24 video to a file, sound to a WAV (the reference video bytes).
"$transcode" --input "$work/m.ae7" --output "$work/raw.rgb" \
    --audio-output "$work/raw.wav" >/dev/null 2>&1

# NUT mode: muxed single stream.
"$transcode" --input "$work/m.ae7" --output-format nut \
    --output "$work/m.nut" >/dev/null 2>&1

# ffmpeg must accept the container and round-trip the video unchanged (rawvideo
# rgb24 in -> rgb24 out is lossless).
"$ffmpeg" -v error -i "$work/m.nut" -map 0:v:0 -f rawvideo -pix_fmt rgb24 \
    "$work/nut.rgb"
if cmp -s "$work/raw.rgb" "$work/nut.rgb"; then
    echo "ok NUT video matches raw RGB24"
else
    echo "FAIL: NUT video differs from raw mode" >&2
    exit 1
fi

# And the muxed audio must decode to the source PCM (signed-16 is lossless).
"$ffmpeg" -v error -i "$work/m.nut" -map 0:a:0 -f s16le -c:a pcm_s16le \
    "$work/nut.pcm"
if cmp -s "$work/expected.pcm" "$work/nut.pcm"; then
    echo "ok NUT audio matches source PCM"
else
    echo "FAIL: NUT audio differs from source" >&2
    exit 1
fi
