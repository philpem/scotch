#!/bin/sh
# Decode our type 20 encoder output on the compiled Acorn Decomp20 and confirm it
# reconstructs exactly the 6Y6UV frame the encoder reported -- proving the type
# 20 delta-coded-chroma stream decodes identically on real Acorn code.
set -eu

fixtures=$1
python=$2
harness=$3
decompressor=$4
work=${TMPDIR:-/tmp}/decomp20-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work"

check_fixture()
{
    stem=$1
    size=$2

    "$python" "$harness" --decompressor "$decompressor" --codec 20 \
        --payload "$work/$stem.mb20" --size "$size" --output-layout 6y6uv \
        --output "$work/$stem.acorn.6y6uv"
    cmp "$work/$stem.expected.6y6uv" "$work/$stem.acorn.6y6uv"
}

check_fixture_prev()
{
    stem=$1
    size=$2

    "$python" "$harness" --decompressor "$decompressor" --codec 20 \
        --payload "$work/$stem.mb20" --size "$size" \
        --previous "$work/$stem.previous.6y6uv" --previous-layout 6y6uv \
        --output-layout 6y6uv --output "$work/$stem.acorn.6y6uv"
    cmp "$work/$stem.expected.6y6uv" "$work/$stem.acorn.6y6uv"
}

check_fixture enc_key 16x16
check_fixture_prev enc_inter 16x16
