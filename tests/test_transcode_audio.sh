#!/bin/sh
# Build a synthetic movie with a signed-16 stereo sound track and confirm
# replay-transcode auto-detects the format from the header and reproduces the
# PCM exactly (signed-16 decode is lossless), with correct stereo framing.
set -eu

maker=$1
transcode=$2

work=${TMPDIR:-/tmp}/transcode-audio-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

"$maker" "$work/m.ae7" "$work/expected.pcm"

# Decode audio to WAV; video to /dev/null. No --audio-format: must auto-detect.
"$transcode" --input "$work/m.ae7" --output /dev/null \
    --audio-output "$work/out.wav"

# Strip the 44-byte WAV header and compare the PCM payload byte-for-byte.
dd if="$work/out.wav" bs=1 skip=44 of="$work/out.pcm" 2>/dev/null

if cmp -s "$work/expected.pcm" "$work/out.pcm"; then
    echo "ok signed-16 stereo audio matches expected"
else
    echo "FAIL audio PCM differs" >&2
    exit 1
fi
