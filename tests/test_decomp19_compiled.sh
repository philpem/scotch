#!/bin/sh
set -eu

encoder=$1
verify=$2
python=$3
harness=$4
decompressor=$5
work=${TMPDIR:-/tmp}/decomp19-compiled-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/frames"

# Two identical frames exercise a data-coded key frame followed by stationary.
: > "$work/input.rgb"
i=0
while [ "$i" -lt 32 ]; do
    printf '\000\000\000' >> "$work/input.rgb"
    i=$((i + 1))
done
"$encoder" --codec 19 --input "$work/input.rgb" --size 4x4 --frames 2 \
    --payload-prefix "$work/frames/frame-" >/dev/null

"$python" "$harness" --decompressor "$decompressor" \
    --payload "$work/frames/frame-000000.mb19" --size 4x4 \
    --output "$work/acorn-0.6y5uv"
"$verify" --codec 19 --payload "$work/frames/frame-000000.mb19" --size 4x4 \
    --expect-6y5uv "$work/acorn-0.6y5uv"

"$python" "$harness" --decompressor "$decompressor" \
    --payload "$work/frames/frame-000001.mb19" --size 4x4 \
    --previous "$work/acorn-0.6y5uv" --output "$work/acorn-1.6y5uv"
"$verify" --codec 19 --payload "$work/frames/frame-000001.mb19" --size 4x4 \
    --previous-6y5uv "$work/acorn-0.6y5uv" \
    --expect-6y5uv "$work/acorn-1.6y5uv"
