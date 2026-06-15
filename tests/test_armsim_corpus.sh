#!/bin/sh
# Decode the format-19 corpus with replay-armsim (the native ARMulator harness)
# and confirm it reproduces the expected Acorn output byte-for-byte. Unlike the
# Unicorn-based Python tests this needs no external dependency, so it always
# runs.
set -eu

armsim=$1
corpus=$2
decompressor=$3

if [ ! -f "$decompressor" ]; then
    echo "SKIP: compiled Decomp19 not found at $decompressor" >&2
    exit 77
fi

work=${TMPDIR:-/tmp}/armsim-corpus-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

fail=0
# Columns: name width height payload previous_6y5uv expected_6y5uv provenance
while IFS=$(printf '\t') read -r name width height payload previous expected _rest; do
    case "$name" in \#*|"") continue ;; esac

    prev_arg=""
    if [ "$previous" != "-" ]; then
        prev_arg="--previous $corpus/$previous"
    fi

    # shellcheck disable=SC2086
    "$armsim" --decompressor "$decompressor" --codec 19 \
        --payload "$corpus/$payload" --size "${width}x${height}" \
        $prev_arg --output "$work/$name.out"

    if cmp -s "$corpus/$expected" "$work/$name.out"; then
        echo "ok $name (${width}x${height})"
    else
        echo "FAIL $name (${width}x${height})"
        fail=1
    fi
done < "$corpus/manifest.tsv"

exit "$fail"
