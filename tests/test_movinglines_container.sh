#!/bin/sh
# Mux a Moving Lines movie straight to a .ae7 container and confirm the result
# parses as a valid ARMovie with the expected structure. (The frames themselves
# are cross-checked against the compiled module by test_movinglines_compiled.)
set -eu

encode=$1
inspect=$2
work=${TMPDIR:-/tmp}/movinglines-container-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# Six 32x16 RGB24 frames; content is arbitrary (the encoder self-checks).
head -c $((32 * 16 * 3 * 6)) /dev/urandom > "$work/in.rgb"

"$encode" --codec 1 --input "$work/in.rgb" --size 32x16 \
    --output "$work/movie,ae7" --fps 12.5 --frames-per-chunk 4 --pad-to-multiple 4

"$inspect" "$work/movie,ae7" > "$work/info.txt"
cat "$work/info.txt"
grep -q "video codec: 1 (Moving Lines)" "$work/info.txt"
grep -q "video: 32x16," "$work/info.txt"
# 6 frames padded up to 8 = two full chunks of four.
grep -q "chunks: 2 entries" "$work/info.txt"
grep -q "4 frames/chunk" "$work/info.txt"
# A poster sprite must be present (a zero-length one crashes !ARPlayer).
grep -qE "sprite: offset=[0-9]+ bytes=[1-9]" "$work/info.txt"
echo "replay-encode --codec 1 --output container ok"
