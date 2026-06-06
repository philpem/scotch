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

## Future Policy Selection

The intended command-line shape is:

```text
--policy portable
--policy acorn
```

`portable` will remain the default policy unless measurements show a material
regression. `acorn` may later reproduce Acorn's cross-family best-error search
and tie behavior for comparison or archival output. Both policies must emit
the same type 19, Super Moving Blocks bitstream syntax and use the same quality
acceptance table.

## Source Provenance Limit

The received `LionFish19,ae7` cannot yet be tied numerically to a source movie.
Both plausible bundled candidates were decoded with compiled Decomp7:

- `LionFish2,ae7`, type 7 (Moving Blocks), 160x128 at 12.5 fps;
- `HiRes/LionFish,ae7`, type 7 (Moving Blocks), 160x128 at 25 fps, sampled at
  half rate.

Neither produces credible native-domain error against `LionFish19` output,
under either literal unpatched-word interpretation or documented YUV555 to
6Y5UV component conversion. Supplying the type 7 key image does not change
chunk 0 because its first frame is self-contained. Therefore no bitrate or
quality parity number is claimed until the exact source path and compressor
command are known.
