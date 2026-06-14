#!/bin/sh
# Smoke-test the replay-movinglines CLI: encode RGB24 frames (with the built-in
# round-trip self-check) and decode a key frame back, confirming the decoded PPM
# matches the encoder's reconstruction.
set -eu

bin=$1
work=${TMPDIR:-/tmp}/movinglines-cli-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Two 8x4 RGB24 frames. Content is arbitrary; the encoder self-checks each frame.
head -c $((8 * 4 * 3 * 2)) /dev/urandom > "$work/in.rgb"

"$bin" encode --input "$work/in.rgb" --size 8x4 \
    --payload-prefix "$work/p" --recon-prefix "$work/r"
test -f "$work/p000000.mln"
test -f "$work/p000001.mln"
test -f "$work/r000000.ppm"

# The first frame is a key frame, so it decodes without a previous frame.
"$bin" decode --payload "$work/p000000.mln" --size 8x4 --output-ppm "$work/d0.ppm"
cmp "$work/r000000.ppm" "$work/d0.ppm"
echo "replay-movinglines CLI ok"
