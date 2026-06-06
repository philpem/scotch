#!/bin/sh
set -eu

encode=$1
verify=$2
work=${TMPDIR:-/tmp}/replay-encode-cli-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

# One packed 6Y5UV 4x4 frame. Direct input avoids an RGB conversion when a
# comparison already has source samples in the codec's native working space.
: > "$work/source.6y5uv"
i=0
while [ "$i" -lt 16 ]; do
    printf '\012\003\005' >> "$work/source.6y5uv"
    i=$((i + 1))
done

"$encode" --codec 19 --input "$work/source.6y5uv" \
    --input-format 6y5uv --size 4x4 --payload "$work/frame.mb19"
"$verify" --codec 19 --size 4x4 --payload "$work/frame.mb19" \
    --expect-6y5uv "$work/source.6y5uv"

# Native samples outside their stored bit widths must be rejected.
printf '\100\000\000' > "$work/invalid.6y5uv"
i=1
while [ "$i" -lt 16 ]; do
    printf '\000\000\000' >> "$work/invalid.6y5uv"
    i=$((i + 1))
done
if "$encode" --codec 19 --input "$work/invalid.6y5uv" \
    --input-format 6y5uv --size 4x4 --payload "$work/invalid.mb19"; then
    echo "invalid 6Y5UV input was accepted" >&2
    exit 1
fi
