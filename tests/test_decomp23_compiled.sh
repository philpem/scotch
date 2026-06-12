#!/bin/sh
set -eu

python=$1
harness=$2
decompressor=$3
work=${TMPDIR:-/tmp}/replay-decomp23-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work"

# Four identical rows. Each row contains two 22-bit little-endian groups:
# Y0=1,Y1=2,U=3,V=-1 and Y0=63,Y1=0,U=0,V=0.
"$python" -c '
import pathlib
p0 = 1 | (2 << 6) | (3 << 12) | (31 << 17)
p1 = 63
groups = [p0, p1] * 4
stream = sum(group << (22 * index) for index, group in enumerate(groups))
pathlib.Path("'$work'/frame.type23").write_bytes(
    stream.to_bytes(22, "little"))
expected = bytes((1,3,31, 2,3,31, 63,0,0, 0,0,0)) * 4
pathlib.Path("'$work'/expected.6y5uv").write_bytes(expected)
'

"$python" "$harness" --decompressor "$decompressor" --codec 23 \
    --payload "$work/frame.type23" --size 4x4 \
    --output "$work/decoded.6y5uv"
cmp "$work/expected.6y5uv" "$work/decoded.6y5uv"
