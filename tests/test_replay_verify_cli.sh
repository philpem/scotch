#!/bin/sh
set -eu

verify=$1
work=${TMPDIR:-/tmp}/replay-verify-cli-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

# A one-byte zero payload is one stationary 4x4 block plus zero padding.
printf '\000' > "$work/stationary.mb19"
: > "$work/previous.6y5uv"
i=0
while [ "$i" -lt 16 ]; do
    printf '\012\003\005' >> "$work/previous.6y5uv"
    i=$((i + 1))
done

"$verify" --codec 19 --size 4x4 \
    --payload "$work/stationary.mb19" \
    --previous-6y5uv "$work/previous.6y5uv" \
    --output-6y5uv "$work/decoded.6y5uv" \
    --expect-6y5uv "$work/previous.6y5uv"
cmp "$work/previous.6y5uv" "$work/decoded.6y5uv"
