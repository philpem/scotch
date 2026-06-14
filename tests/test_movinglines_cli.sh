#!/bin/sh
# Smoke-test replay-encode --codec 1 (Moving Lines): encode RGB24 frames -- the
# encoder round-trip self-checks each frame internally -- and confirm it writes
# the per-frame payloads and reconstruction PPMs.
set -eu

encode=$1
work=${TMPDIR:-/tmp}/movinglines-cli-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Two 8x4 RGB24 frames. Content is arbitrary; the encoder self-checks each frame.
head -c $((8 * 4 * 3 * 2)) /dev/urandom > "$work/in.rgb"

"$encode" --codec 1 --input "$work/in.rgb" --size 8x4 \
    --payload-prefix "$work/p" --recon-prefix "$work/r"
test -f "$work/p000000.mln"
test -f "$work/p000001.mln"
test -f "$work/r000000.ppm"
test -f "$work/r000001.ppm"
echo "replay-encode --codec 1 CLI ok"
