#!/bin/sh
# Build a synthetic type-23 movie and confirm replay-transcode decodes it (no ARM
# module needed) to the expected RGB24 stream, exercising the multi-chunk frame
# walk and the 6Y6Y5U5V unpack path.
set -eu

maker=$1
transcode=$2

work=${TMPDIR:-/tmp}/transcode-t23-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# 16x8, 5 frames, 2 frames/chunk -> chunks of 2,2,1 (tests a short final chunk).
"$maker" "$work/m.ae7" "$work/expected.rgb" 16 8 5 2

"$transcode" --input "$work/m.ae7" --output "$work/out.rgb"

if cmp -s "$work/expected.rgb" "$work/out.rgb"; then
    echo "ok type23 transcode matches expected"
else
    echo "FAIL type23 transcode output differs" >&2
    exit 1
fi
