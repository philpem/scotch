#!/bin/sh
set -eu

fixtures=$1
verify=$2
python=$3
harness=$4
decompressor=$5
work=${TMPDIR:-/tmp}/decomp19-compiled-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work"

check_fixture()
{
    stem=$1
    size=$2
    previous=$3

    if [ -n "$previous" ]; then
        "$python" "$harness" --decompressor "$decompressor" \
            --payload "$work/$stem.mb19" --size "$size" \
            --previous "$work/$previous" \
            --output "$work/$stem.acorn.6y5uv"
        "$verify" --codec 19 --payload "$work/$stem.mb19" --size "$size" \
            --previous-6y5uv "$work/$previous" \
            --expect-6y5uv "$work/$stem.acorn.6y5uv"
    else
        "$python" "$harness" --decompressor "$decompressor" \
            --payload "$work/$stem.mb19" --size "$size" \
            --output "$work/$stem.acorn.6y5uv"
        "$verify" --codec 19 --payload "$work/$stem.mb19" --size "$size" \
            --expect-6y5uv "$work/$stem.acorn.6y5uv"
    fi
    cmp "$work/$stem.expected.6y5uv" "$work/$stem.acorn.6y5uv"
}

check_fixture temporal4x4 8x4 temporal4x4.previous.6y5uv
check_fixture spatial4x4 8x4 ""
check_fixture temporal2x2 8x4 temporal2x2.previous.6y5uv
check_fixture spatial2x2 8x4 ""
check_fixture lossy-split 4x4 lossy-split.previous.6y5uv
