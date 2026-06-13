#!/bin/sh
# Standing full-movie gate: encode a multi-frame synthetic scene with the real
# encoder, then decode every frame on the genuine compiled Decomp module and
# compare byte-for-byte against the encoder's own reconstruction. Each inter
# frame is decoded against the previous frame's reconstruction, so a divergence
# is reported at the exact frame it first appears rather than cascading.
#
# This is the coverage the focused fixtures lacked: it runs real, multi-block,
# multi-frame content through both decoders, the check that catches table or
# grammar bugs the encoder and portable verifier would otherwise agree on.
set -eu

fixtures=$1
python=$2
harness=$3
decompressor=$4
codec=$5
layout=$6   # working-space layout for this codec: yuv555 or 6y5uv
frames=8

work=${TMPDIR:-/tmp}/fullmovie-$codec-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work" "$codec"

f=0
while [ "$f" -lt "$frames" ]; do
    stem=$(printf 'f%03d' "$f")
    if [ "$f" -eq 0 ]; then
        "$python" "$harness" --decompressor "$decompressor" --codec "$codec" \
            --payload "$work/$stem.payload" --size 80x48 \
            --output-layout "$layout" --output "$work/$stem.acorn"
    else
        prev=$(printf 'f%03d' "$((f - 1))")
        "$python" "$harness" --decompressor "$decompressor" --codec "$codec" \
            --payload "$work/$stem.payload" --size 80x48 \
            --previous "$work/$prev.recon" --previous-layout "$layout" \
            --output-layout "$layout" --output "$work/$stem.acorn"
    fi
    if ! cmp "$work/$stem.recon" "$work/$stem.acorn"; then
        echo "codec $codec: frame $f decodes differently on compiled Decomp$codec" >&2
        exit 1
    fi
    f=$((f + 1))
done
echo "codec $codec: $frames frames match compiled Decomp$codec"
