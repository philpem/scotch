#!/bin/sh
set -eu

python=$1
harness=$2
if ! "$python" -c 'import unicorn' >/dev/null 2>&1; then
    exit 77
fi

work=${TMPDIR:-/tmp}/decomp19-unicorn-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

# Header-compatible synthetic ARM image:
#   +4  MOV pc,lr                         (init)
#   +8  LDMIA r2!,{r5-r12}; STMIA r1!,... (copy 16 previous pixels)
printf '\000\000\000\000\016\360\240\341' > "$work/decompressor"
printf '\340\037\262\350\340\037\241\350' >> "$work/decompressor"
printf '\340\037\262\350\340\037\241\350' >> "$work/decompressor"
printf '\016\360\240\341' >> "$work/decompressor"
# Unreachable signatures used to install the classic-alignment hooks. The
# generated decoder contains four block-header loads and two Huffman loads.
printf '\140\000\227\350\140\000\227\350' >> "$work/decompressor"
printf '\140\000\227\350\140\000\227\350' >> "$work/decompressor"
printf '\100\001\226\350\300\000\226\350' >> "$work/decompressor"
printf '\000' > "$work/payload"
: > "$work/previous.6y5uv"
i=0
while [ "$i" -lt 16 ]; do
    printf '\012\003\005' >> "$work/previous.6y5uv"
    i=$((i + 1))
done

"$python" "$harness" --decompressor "$work/decompressor" \
    --payload "$work/payload" --size 4x4 \
    --previous "$work/previous.6y5uv" --output "$work/output.6y5uv"
cmp "$work/previous.6y5uv" "$work/output.6y5uv"

# Repeated mode must carry the first reconstruction into the second call and
# expose one numbered output and payload slice per decoder return.
printf '\000' > "$work/payload-two"
"$python" "$harness" --decompressor "$work/decompressor" \
    --payload "$work/payload-two" --size 4x4 --frames 2 \
    --previous "$work/previous.6y5uv" \
    --output-prefix "$work/frame-" --payload-prefix "$work/payload-"
cmp "$work/previous.6y5uv" "$work/frame-000000.6y5uv"
cmp "$work/frame-000000.6y5uv" "$work/frame-000001.6y5uv"
test -f "$work/payload-000000.mb19"
test -f "$work/payload-000001.mb19"

# Format 7, Moving Blocks uses five instances of the same classic unaligned
# header load and no format-19 Huffman lookahead signatures.
printf '\000\000\000\000\016\360\240\341' > "$work/decompressor7"
printf '\340\037\262\350\340\037\241\350' >> "$work/decompressor7"
printf '\340\037\262\350\340\037\241\350' >> "$work/decompressor7"
printf '\016\360\240\341' >> "$work/decompressor7"
printf '\140\000\227\350\140\000\227\350' >> "$work/decompressor7"
printf '\140\000\227\350\140\000\227\350' >> "$work/decompressor7"
printf '\140\000\227\350' >> "$work/decompressor7"
"$python" "$harness" --codec 7 --decompressor "$work/decompressor7" \
    --payload "$work/payload" --size 4x4 \
    --previous "$work/previous.6y5uv" --output "$work/output7.6y5uv"
cmp "$work/previous.6y5uv" "$work/output7.6y5uv"
