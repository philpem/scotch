#!/bin/sh
# Exercise the block-rounding path: build a type-23 movie whose frames are packed
# at 16 wide but whose header declares 12 (not a multiple of Decomp23's 16-pixel
# block). The module route must round 12 -> 16 to decode the 16-wide bitstream,
# then crop back to 12 on output. Without the rounding the decode desyncs and the
# crop is wrong; this guards both. The maker emits the expected 12-wide RGB.
set -eu

maker=$1
transcode=$2
module=$3

if [ ! -f "$module" ]; then
    echo "SKIP: compiled Decomp23 not present at $module" >&2
    exit 77
fi

work=${TMPDIR:-/tmp}/transcode-mod23rnd-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# pack at 16, declare 12 -> transcoder rounds to 16, crops to 12
"$maker" "$work/m.ae7" "$work/expected.rgb" 16 8 5 2 12
"$transcode" --output-format raw --input "$work/m.ae7" --module "$module" --output "$work/out.rgb"

if cmp -s "$work/expected.rgb" "$work/out.rgb"; then
    echo "ok type23 rounding 12->16 via Decomp23 module matches cropped native"
else
    echo "FAIL rounded module-route output differs" >&2
    exit 1
fi
