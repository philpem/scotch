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
