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
# Default RGB packing declares the RGB16 colour map (label [RGB] + 16-bit depth).
head -c 256 "$work/movie,ae7" | grep -q "16 bits per pixel \[RGB\]"

# --colour yuv packs YUV555 and declares the YUV16 colour map ([YUV] + depth),
# so the player loads MovingLine.ColourMap.YUV16.
"$encode" --codec 1 --colour yuv --input "$work/in.rgb" --size 32x16 \
    --output "$work/yuv,ae7" --fps 12.5 --frames-per-chunk 4 --pad-to-multiple 4
head -c 256 "$work/yuv,ae7" | grep -q "16 bits per pixel \[YUV\]"

# Regression: the final chunk must be FULL even without --pad-to-multiple. The
# player decodes a fixed frames-per-chunk from every chunk, so an under-filled
# last chunk is decoded past its data into garbage (frozen/stale frames at the
# end of playback). The container path therefore pads to whole chunks itself.
# Encode 10 frames at 4/chunk with audio and NO --pad-to-multiple: the sink must
# be padded to 12 (three full chunks of 4), so every chunk -- including the last
# -- carries the same full sound payload.
head -c $((32 * 16 * 3 * 10)) /dev/urandom > "$work/ten.rgb"
head -c $((11025 * 2)) /dev/zero > "$work/silence.pcm"   # 1 s mono s16le, ample
"$encode" --codec 1 --input "$work/ten.rgb" --size 32x16 \
    --output "$work/pad,ae7" --fps 12.5 --frames-per-chunk 4 \
    --audio-input "$work/silence.pcm" --sound-encode vidc-e8 \
    --sound-rate 11025 --sound-channels 1
"$inspect" "$work/pad,ae7" > "$work/padinfo.txt"
cat "$work/padinfo.txt"
grep -q "chunks: 3 entries" "$work/padinfo.txt"        # 10 frames padded up to 12
grep -q "4 frames/chunk" "$work/padinfo.txt"
first_snd=$(sed -n 's/^chunk 0:.* sound=\([0-9]*\) .*/\1/p' "$work/padinfo.txt")
last_snd=$(grep '^chunk ' "$work/padinfo.txt" | tail -1 | \
    sed -n 's/.* sound=\([0-9]*\) .*/\1/p')
[ -n "$first_snd" ] && [ "$first_snd" -gt 0 ] && [ "$first_snd" = "$last_snd" ] || {
    echo "final chunk is under-filled: first=$first_snd last=$last_snd" >&2
    exit 1
}
echo "replay-encode --codec 1 --output container ok (rgb + yuv + full-tail-chunk)"
