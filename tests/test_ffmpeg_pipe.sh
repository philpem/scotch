#!/bin/sh
set -eu

ffmpeg=$1
encode=$2
verify=$3
work=${TMPDIR:-/tmp}/replay-ffmpeg-pipe-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

"$ffmpeg" -v error -f lavfi -i testsrc2=size=16x16:rate=2 \
    -frames:v 2 -an -pix_fmt rgb24 -f rawvideo - | \
    "$encode" --codec 19 --input - --input-format rgb24 --size 16x16 \
        --frames 2 --payload-prefix "$work/frame-" --loss-level 7

test -s "$work/frame-000000.mb19"
test -s "$work/frame-000001.mb19"
"$verify" --codec 19 --size 16x16 --payload "$work/frame-000000.mb19"
