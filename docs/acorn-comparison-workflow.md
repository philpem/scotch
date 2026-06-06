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
