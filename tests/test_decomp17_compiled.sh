#!/bin/sh
set -eu

python=$1
harness=$2
decompressor=$3
work=${TMPDIR:-/tmp}/replay-decomp17-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

# One data-coded 4x4: U=3, V=5, first residual +1, then fifteen zeroes.
# The two-dimensional predictor reconstructs every luma sample as 1.
printf '\215\162\125\125\125\025' > "$work/frame.mb17"
"$python" -c '
from pathlib import Path
Path("'$work'/expected.yuv555").write_bytes(bytes((1, 3, 5)) * 16)
'

"$python" "$harness" --decompressor "$decompressor" --codec 17 \
    --payload "$work/frame.mb17" --size 4x4 --output-layout yuv555 \
    --output "$work/decoded.yuv555"
cmp "$work/expected.yuv555" "$work/decoded.yuv555"

# One split 4x4 containing four zero-residual data-coded 2x2 quadrants. The
# predictor remains zero; distinct chroma values verify TL,TR,BL,BR placement.
"$python" -c '
from pathlib import Path
value = 3
position = 2
for u, v in ((1,5), (2,6), (3,7), (4,8)):
    header = 2 | (u << 2) | (v << 7)
    value |= header << position
    position += 12
    for unused in range(4):
        value |= 2 << position
        position += 2
Path("'$work'/split.mb17").write_bytes(
    value.to_bytes((position + 7) // 8, "little"))
pixels = bytearray()
for y in range(4):
    for x in range(4):
        quadrant = (2 if y >= 2 else 0) + (1 if x >= 2 else 0)
        u, v = ((1,5), (2,6), (3,7), (4,8))[quadrant]
        pixels += bytes((0, u, v))
Path("'$work'/split-expected.yuv555").write_bytes(pixels)
'
"$python" "$harness" --decompressor "$decompressor" --codec 17 \
    --payload "$work/split.mb17" --size 4x4 --output-layout yuv555 \
    --output "$work/split-decoded.yuv555"
cmp "$work/split-expected.yuv555" "$work/split-decoded.yuv555"

# Exercise the copy grammar independently of the portable verifier. The first
# 4x4 is stationary. The second is either a temporal copy from x-1 or a spatial
# copy from x-4 in the already reconstructed current frame.
"$python" -c '
from pathlib import Path
previous = bytes(
    component
    for i in range(32)
    for component in (i & 31, (i + 3) & 31, (i + 7) & 31))
Path("'$work'/previous-8x4.yuv555").write_bytes(previous)
pixels = [previous[i:i + 3] for i in range(0, len(previous), 3)]
previous4 = [pixel for y in range(4) for pixel in pixels[y * 8:y * 8 + 4]]
Path("'$work'/previous-4x4.yuv555").write_bytes(b"".join(previous4))

def write_fields(fields):
    value = 0
    position = 0
    for field, width in fields:
        value |= field << position
        position += width
    return value.to_bytes((position + 7) // 8, "little")

# stationary; motion; radius-one family; ring index 3 == (-1, 0)
Path("'$work'/temporal.mb17").write_bytes(
    write_fields(((0,2), (2,2), (0,2), (3,3))))
temporal = []
for y in range(4):
    temporal += pixels[y * 8:y * 8 + 4]
    temporal += pixels[y * 8 + 3:y * 8 + 7]
Path("'$work'/temporal-expected.yuv555").write_bytes(b"".join(temporal))

# stationary; motion; shared spatial/radius-three family; spatial index 5
Path("'$work'/spatial.mb17").write_bytes(
    write_fields(((0,2), (2,2), (1,2), (5,5))))
spatial = []
for y in range(4):
    row = pixels[y * 8:y * 8 + 4]
    spatial += row + row
Path("'$work'/spatial-expected.yuv555").write_bytes(b"".join(spatial))

# Split children: stationary TL, temporal (-1,0) TR, stationary BL, then
# spatial (-2,0), index 6, BR. The final reference is legal because BL has
# already been reconstructed in the same parent.
Path("'$work'/split-copy.mb17").write_bytes(write_fields((
    (3,2), (0,2), (1,1), (0,2), (3,3),
    (0,2), (1,1), (1,2), (6,5))))
split = []
for y in range(4):
    if y < 2:
        split += pixels[y * 8:y * 8 + 2]
        split += pixels[y * 8 + 1:y * 8 + 3]
    else:
        row = pixels[y * 8:y * 8 + 2]
        split += row + row
Path("'$work'/split-copy-expected.yuv555").write_bytes(b"".join(split))
'

for fixture in temporal spatial; do
    "$python" "$harness" --decompressor "$decompressor" --codec 17 \
        --payload "$work/$fixture.mb17" --size 8x4 \
        --previous "$work/previous-8x4.yuv555" --previous-layout yuv555 \
        --output-layout yuv555 \
        --output "$work/$fixture-decoded.yuv555"
    cmp "$work/$fixture-expected.yuv555" "$work/$fixture-decoded.yuv555"
done

"$python" "$harness" --decompressor "$decompressor" --codec 17 \
    --payload "$work/split-copy.mb17" --size 4x4 \
    --previous "$work/previous-4x4.yuv555" --previous-layout yuv555 \
    --output-layout yuv555 \
    --output "$work/split-copy-decoded.yuv555"
cmp "$work/split-copy-expected.yuv555" "$work/split-copy-decoded.yuv555"
