#!/bin/sh
# End-to-end transcoder check: decode the LionFish19 movie to raw RGB24 and
# confirm the stream is whole frames and that its first two frames match the
# format-19 corpus (the same frames replay-armsim is anchored against).
set -eu

transcode=$1
checker=$2
movie=$3
modules_dir=$4
corpus=$5

module="$modules_dir/Decomp19/Decompress,ffd"
if [ ! -f "$movie" ] || [ ! -f "$module" ]; then
    echo "SKIP: LionFish19 movie or compiled Decomp19 not present" >&2
    exit 77
fi

work=${TMPDIR:-/tmp}/transcode-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

width=160
height=128
"$transcode" --output-format raw --input "$movie" --modules-dir "$modules_dir" \
    --output "$work/out.rgb"

size=$(wc -c < "$work/out.rgb")
frame_bytes=$((width * height * 3))
if [ "$size" -eq 0 ] || [ $((size % frame_bytes)) -ne 0 ]; then
    echo "FAIL: output ($size bytes) is not a whole number of ${width}x${height} frames" >&2
    exit 1
fi
echo "ok stream: $((size / frame_bytes)) frames of ${width}x${height}"

"$checker" "$work/out.rgb" "$width" "$height" 0 "$corpus/lionfish19-c00-f00.6y5uv"
"$checker" "$work/out.rgb" "$width" "$height" 1 "$corpus/lionfish19-c00-f01.6y5uv"
