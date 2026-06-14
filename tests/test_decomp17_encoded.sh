#!/bin/sh
# Decode our type 17 encoder output on the compiled Acorn Decomp17 and confirm
# it reconstructs exactly the YUV555 frame the encoder reported.
set -eu

fixtures=$1
python=$2
harness=$3
decompressor=$4
if ! "$python" -c 'import unicorn' >/dev/null 2>&1; then
    echo "SKIP: the Python Unicorn bindings are required" >&2
    exit 77
fi
work=${TMPDIR:-/tmp}/decomp17-encoded-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work"

check_fixture()
{
    stem=$1
    size=$2

    if [ -f "$work/$stem.previous.yuv555" ]; then
        "$python" "$harness" --decompressor "$decompressor" --codec 17 \
            --payload "$work/$stem.mb17" --size "$size" \
            --previous "$work/$stem.previous.yuv555" --previous-layout yuv555 \
            --output-layout yuv555 --output "$work/$stem.acorn.yuv555"
    else
        "$python" "$harness" --decompressor "$decompressor" --codec 17 \
            --payload "$work/$stem.mb17" --size "$size" --output-layout yuv555 \
            --output "$work/$stem.acorn.yuv555"
    fi
    cmp "$work/$stem.expected.yuv555" "$work/$stem.acorn.yuv555"
}

check_fixture data4x4 4x4
check_fixture data8x8 8x8
check_fixture data-wrap 4x4
check_fixture spatial4x4 8x4
check_fixture stationary4x4 8x4
check_fixture temporal4x4 8x4
check_fixture split4x4 4x4
