#!/bin/sh
set -eu

python=$1
sweep=$2
encode=$3
verify=$4
work=${TMPDIR:-/tmp}/mb19-quality-sweep-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/source"

# Two 4x4 native frames. The second differs slightly so lossy policy choices
# have temporal candidates to consider without making the test expensive.
frame=0
while [ "$frame" -lt 2 ]; do
    output=$(printf '%s/frame-%06d.6y5uv' "$work/source" "$frame")
    : > "$output"
    pixel=0
    while [ "$pixel" -lt 16 ]; do
        y=$((10 + frame + pixel % 4))
        octal=$(printf '%03o' "$y")
        printf "\\$octal\003\005" >> "$output"
        pixel=$((pixel + 1))
    done
    frame=$((frame + 1))
done

"$python" "$sweep" --encode "$encode" --verify "$verify" \
    --source-prefix "$work/source/frame-" --frames 2 --size 4x4 \
    --levels 0,7 --policies lowest-error ordered \
    --work-dir "$work/results" --output "$work/sweep.md"

grep -F '| lowest-error | level 0 |' "$work/sweep.md" >/dev/null
grep -F '| lowest-error | level 7 |' "$work/sweep.md" >/dev/null
grep -F '| ordered | level 0 |' "$work/sweep.md" >/dev/null
grep -F '| ordered | level 7 |' "$work/sweep.md" >/dev/null
test -s "$work/results/lowest-error-q07/verify.report"
test -s "$work/results/ordered-q07/encode.trace"

"$python" "$sweep" --encode "$encode" --verify "$verify" \
    --source-prefix "$work/source/frame-" --frames 2 --size 4x4 \
    --targets 16 --initial-level 7 --policies lowest-error \
    --work-dir "$work/targets" --output "$work/targets.md"
grep -F '| lowest-error | target 16 |' "$work/targets.md" >/dev/null
test -s "$work/targets/lowest-error-target-16/encode.trace"
