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

# Policy selection is explicit even though a single key-frame data block has
# no copy candidates whose choice could differ.
"$encode" --codec 19 --input "$work/source.6y5uv" \
    --input-format 6y5uv --size 4x4 --payload "$work/policy.mb19" \
    --policy lowest-error
cmp "$work/frame.mb19" "$work/policy.mb19"

# Bracketed target search must still emit a verifier-clean payload. This tiny
# key frame is not rate controlled, so include two frames.
cat "$work/source.6y5uv" "$work/source.6y5uv" > "$work/two.6y5uv"
mkdir "$work/target"
"$encode" --codec 19 --input "$work/two.6y5uv" \
    --input-format 6y5uv --size 4x4 --frames 2 \
    --payload-prefix "$work/target/frame-" --target-bytes 2 \
    --rate-search bracketed
test -s "$work/target/frame-000001.mb19"

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
