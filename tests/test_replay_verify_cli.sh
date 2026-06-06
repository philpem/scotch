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
    --expect-6y5uv "$work/previous.6y5uv" \
    --reference-6y5uv "$work/previous.6y5uv" \
    --trace "$work/trace.txt" --summary > "$work/output.txt"
cmp "$work/previous.6y5uv" "$work/decoded.6y5uv"
grep -q 'sse_y=0 sse_u=0 sse_v=0 .*psnr_y=inf' "$work/output.txt"
grep -q 'summary .*stationary4x4=1 .*stationary4x4_bits=2 .*split_header_bits=0 .*semantic_bits=2 payload_bytes=1' "$work/output.txt"
printf '%s\n' \
    'codec=19 name="Super Moving Blocks" x=0 y=0 size=4 mode=stationary bit_start=0 bit_end=2 bits=2 dx=0 dy=0 sse_y=0 sse_u=0 sse_v=0 mse_y=0.000000 mse_u=0.000000 mse_v=0.000000 max_error_y=0 max_error_u=0 max_error_v=0' \
    > "$work/expected-trace.txt"
cmp "$work/expected-trace.txt" "$work/trace.txt"
