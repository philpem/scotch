# Acorn Encoder Comparison Workflow

Exact Acorn encoder parity is optional. This workflow measures whether the
portable type 19, Super Moving Blocks encoder delivers comparable bitrate and
decoder-visible quality, and explains differences through per-block traces.

## First Requested Sample

Use this bundled source movie:

```text
RiscOS_2003/RiscOS/UnU/Demos/Videos/ARMovies/LionFish2,ae7
```

It is type 7, Moving Blocks, and contains natural motion and fine detail. Run
the Acorn `Decomp19` `BatchComp` compressor to produce type 19, Super Moving
Blocks with these settings:

- fixed `-Quality 7`;
- `-NoDither`, matching the portable encoder's current CompLib conversion;
- original width, height, and frame rate;
- normal key-frame handling; do not use `-NoKeys`;
- process the complete source movie.

The CompLib option set is therefore:

```text
-Source <LionFish2> -Dest <destination>. -Quality 7 -NoDither -Batch
```

The exact program prefix depends on how `Decomp19/BatchComp` is installed in
the emulator. Preserve the complete destination/output, its `Log` file, and
the exact command line. BatchComp may create intermediate files as part of the
Replay build process; preserve the destination directory rather than only the
final file.

## Received Sample

The recompressed movie is present as:

```text
../LionFish19,ae7
```

Recorded properties:

```text
size     3,554,760 bytes
SHA-256  e4a6539b19a105e80e3171a4753870b184edafded0ee874bf2f470231b661684
file     Acorn Replay ARMovie
```

Its AE7 metadata and first chunk have been parsed. Compiled Decomp19 establishes
25 exact frame boundaries in chunk 0; the catalogue's 181,886-byte video area
contains 181,885 bytes consumed by frames plus one byte of alignment padding.

The Acorn compressor's two-pane display and coloured debug block map are
documented in the project-level note
`notes/super-moving-blocks-compressor-display.md`.

## Portable Comparison

Encode the same decoded RGB24 frame sequence with fixed loss level 7:

```sh
ffmpeg -i source-video -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size WIDTHxHEIGHT \
    --payload-prefix portable/frame- --loss-level 7 \
    --trace portable/frames.txt
```

Encoder traces now include payload size, mode counts, native 6Y5UV SSE/MSE,
PSNR, and maximum component error for every attempted frame.

Use `--input-format 6y5uv` when the source has already been decoded to native
samples. `tools/mb19_compare_reports.py` aggregates verifier reports using
summed squared error and sample counts; it does not average per-frame PSNR.

Decode and trace an extracted type 19, Super Moving Blocks payload with:

```sh
build/replay-verify --codec 19 --payload frame.mb19 --size WIDTHxHEIGHT \
    --previous-6y5uv previous.6y5uv --output-6y5uv decoded.6y5uv \
    --reference-6y5uv source.6y5uv --trace blocks.txt
```

The block trace records mode, coordinates, motion vector, exact bit range, and,
when `--reference-6y5uv` is present, native-domain error for that block. For
split blocks, four child records are followed by one enclosing split record.
The enclosing range must not be added to the child sizes when totaling bits;
it exists to make the hierarchy explicit.

Five-bit U/V values are converted from their modulo representation to signed
`-16..15` values before error is measured. Y uses its native `0..63` range.
This makes the reported metrics properties of the codec reconstruction rather
than of a later RGB conversion.

## Policy Selection

The intended command-line shape is:

```text
--policy lowest-error
--policy ordered
```

`lowest-error` is the default and compares accepted copy candidates by
decoder-visible error, emitted bits, then stable family/table order. `ordered`
retains the earlier portable stationary, temporal, spatial family priority.
An exact Acorn-compatible policy may still be added later for archival output.
All policies emit the same type 19 (Super Moving Blocks) syntax and use the
same source-derived quality table.

## Confirmed Source And Type 2 Intermediate

The source is confirmed as `LionFish2,ae7`, type 7 (Moving Blocks), 160x128 at
12.5 fps. Its exact size is 3,598,406 bytes and its SHA-256 is
`1aca14e46dcd9b3ac797d4c7145ad26fa6a3855619cd43d8eee5d24da44f4328`.

`LionFishT2,ae7` was generated as type 2 (16 bit colour uncompressed) with
`-Convert 6Y5UV`. Its structure and frame sizes are valid, but it does not play
correctly because CompLib did not convert the type 7 source words. CompLib only
enters its component-conversion path when the source codec supplies `Dec24`;
Decomp7 has no `Dec24`. The output payload therefore remains YUV555 while the
header says `[6Y5UV]`, making Replay apply the wrong colour mapping.

This is measurable in chunk 0: none of 512,000 halfwords has bit 15 set, all
three YUV555 fields span `0..31`, and interpreting the same words as 6Y5UV
restricts V to `0..15`. The incorrect header accounts for the visible blocks
of extreme colour.

The payload is nevertheless the authoritative word stream presented to the
type 19 (Super Moving Blocks) compressor. Interpreting those words through the
type 19 field layout and comparing the 25 chunk-0 frames gives Acorn's output
45.221729 dB luma PSNR with maximum luma error 2. This validates the source
alignment and supersedes the earlier Decomp7-emulation hypothesis.

The corrected intermediate is:

```text
path     /home/philpem/riscos/RPCEmu/rpcemu/hostfs/Public/ReplayComp/LionFishX,ae7
size     16,118,852 bytes
SHA-256  f6a71e4e73dda589d131146ae0de79f4e350fbdcd2fe7bed891e3a39b1b41020
format   type 2, 16 bit colour uncompressed [YUV]
```

It contains 15 chunks of 25 frames, with exactly 1,024,000 video bytes per
chunk. Its YUV555 payload is full-range and its 25 chunk-0 frames reproduce the
same 45.221729 dB luma PSNR and maximum luma error 2 comparison against
`LionFish19`. This confirms the source pixels and frame alignment while using a
colour-space declaration that Replay can display correctly.

Extract all 375 fixed-size source frames with:

```sh
mkdir -p source
build/replay-extract --input LionFishX,ae7 \
    --output-prefix source/frame- --type2-layout type19-fields
```

The layout name is intentionally explicit. It maps halfword bits directly to
the fields consumed by type 19 (Super Moving Blocks); it does not perform a
YUV555-to-6Y5UV colour conversion. Extracted frames 0 through 24 match the
previously validated source corpus byte-for-byte.
