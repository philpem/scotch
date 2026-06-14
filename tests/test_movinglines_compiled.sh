#!/bin/sh
# Decode the Moving Lines (type 1) cross-check fixtures on the genuine compiled
# MovingLine module under Unicorn, and confirm the module reproduces this
# project's codec_movinglines decoder output byte-for-byte.
set -eu

fixtures=$1
python=$2
harness=$3
decompressor=$4
if ! "$python" -c 'import unicorn' >/dev/null 2>&1; then
    echo "SKIP: the Python Unicorn bindings are required" >&2
    exit 77
fi
work=${TMPDIR:-/tmp}/movinglines-compiled-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"
"$fixtures" "$work"

while read -r stem width height hasprev; do
    prev=""
    if [ "$hasprev" = "1" ]; then
        prev="--previous $work/$stem.prev16"
    fi
    # shellcheck disable=SC2086
    "$python" "$harness" --decompressor "$decompressor" \
        --payload "$work/$stem.mln" --size "${width}x${height}" \
        $prev --output "$work/$stem.acorn16"
    cmp "$work/$stem.expected16" "$work/$stem.acorn16"
    echo "ok $stem (${width}x${height})"
done < "$work/manifest"
