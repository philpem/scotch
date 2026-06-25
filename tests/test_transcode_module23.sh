#!/bin/sh
# Validate the uncompressed-format decompressor route: decode the synthetic
# type-23 movie through the real Decomp23 module (--module forces the module
# path instead of the native unpacker) and confirm it matches the expected RGB.
# This exercises running a tiny uncompressed Decompress module under codecif.
set -eu

maker=$1
transcode=$2
module=$3

if [ ! -f "$module" ]; then
    echo "SKIP: compiled Decomp23 not present at $module" >&2
    exit 77
fi

work=${TMPDIR:-/tmp}/transcode-mod23-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

"$maker" "$work/m.ae7" "$work/expected.rgb" 16 8 5 2
"$transcode" --output-format raw --input "$work/m.ae7" --module "$module" --output "$work/out.rgb"

if cmp -s "$work/expected.rgb" "$work/out.rgb"; then
    echo "ok type23 via Decomp23 module matches native unpack"
else
    echo "FAIL module-route output differs" >&2
    exit 1
fi
