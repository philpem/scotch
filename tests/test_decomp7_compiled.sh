#!/bin/sh
# Decode hand-laid type 7 payloads on the compiled Acorn Decomp7 and confirm it
# produces the same YUV555 our verifier did -- anchoring the literal-data format
# and block grammar against real Acorn code.
set -eu

fixtures=$1
python=$2
harness=$3
decompressor=$4
work=${TMPDIR:-/tmp}/decomp7-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work"

check_fixture()
{
    stem=$1
    size=$2

    "$python" "$harness" --decompressor "$decompressor" --codec 7 \
        --payload "$work/$stem.mb7" --size "$size" --output-layout yuv555 \
        --output "$work/$stem.acorn.yuv555"
    cmp "$work/$stem.expected.yuv555" "$work/$stem.acorn.yuv555"
}

check_fixture data 4x4
check_fixture split 4x4
