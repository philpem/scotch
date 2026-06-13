# Replay Video Format Inventory

This note inventories the Acorn-assigned Replay video formats relevant to the
portable encoder project. It combines `Resources/Documents/AE7doc`,
`Docs/Status`, and the individual `Video/Decomp*/Resources/Info` files.

`AE7doc` describes the allocation current when it was written. The 2003 source
tree extends that list with formats 26 and 27, so `Docs/Status` and the `Info`
files are the better source for what was actually present later.

## Acorn Formats

| Type | Name | Coding class | Capabilities / working input |
| ---: | --- | --- | --- |
| 0 | No video | none | no video track |
| 1 | Moving Lines | Acorn inter-frame | Temporal, Spatial; RGB555 or YUV555 |
| 2 | 16-bit colour | uncompressed | RGB555, YUV555, or 6Y5UV |
| 3 | YYUV | uncompressed/subsampled | YUV555 |
| 4 | 8-bit colour | uncompressed | palettised 8-bit |
| 5 | 4Y1UV | uncompressed/subsampled | YUV555 |
| 6 | 16Y1UV | uncompressed/subsampled | YUV555 |
| 7 | Moving Blocks | Acorn inter-frame | Temporal, Spatial; YUV555 |
| 8 | 24-bit colour | uncompressed | RGB888 or YUV888 |
| 9 | YYUV8 | uncompressed/subsampled | YUV888 |
| 10 | 4Y1UV8 | uncompressed/subsampled | YUV888 |
| 11 | 16Y1UV8 | uncompressed/subsampled | YUV888 |
| 12 | MPEG 1 indirected | external/indirect | Temporal; YUV888 |
| 13 | MPEG in ARMovie | standard codec payload | listed by `AE7doc`; no matching source directory here |
| 14 | Ultimotion | external codec | listed by `AE7doc`; later WSS codecs use other allocations |
| 15 | Indirect video | dispatch wrapper | named decompressor follows the type number |
| 16 | 4Y1UV8 | uncompressed/subsampled | YUV888; appears to supersede/duplicate type 10 |
| 17 | Moving Blocks HQ | Acorn inter-frame | Temporal, Spatial; YUV555 |
| 18 | H.263 | standard inter-frame | Temporal, Spatial; YUV input |
| 19 | Super Moving Blocks | Acorn inter-frame | Temporal, Spatial; 6Y5UV |
| 20 | Moving Blocks Beta | Acorn inter-frame | Temporal, Spatial; 6Y6UV |
| 21 | YUYV8 | uncompressed/subsampled | YUV888 |
| 22 | YY8UVd4 | spatial/delta | Spatial; YUV888 |
| 23 | 6Y6Y5U5V | packed uncompressed 4:2:2 | 6Y5UV; horizontal chroma sharing |
| 24 | 4x6Y1x5UV | uncompressed/subsampled | 6Y5UV |
| 25 | YYYYd4UVd4 | delta/subsampled | 6Y6UV |
| 26 | YYYYd3.5UVd3.5 | delta/subsampled | 6Y6UV |
| 27 | YYYYd3UVd3 | delta/subsampled | YUV555 |

Types 2 through 6 and 8 through 11 are useful implementation fixtures: they
provide simple ways to test Replay container writing and colour conversion
without also debugging an inter-frame codec.

## Portable Tooling Support

The current `replay-extract` implementation supports only type 2, 16-bit
colour uncompressed movies. Even that support is intentionally narrow:

```sh
replay-extract --input movie,ae7 --output-prefix source/frame- \
    --type2-layout type19-fields
```

`--type2-layout type19-fields` reads each stored little-endian halfword as
`Y=word[5:0]`, `U=word[10:6]`, and `V=word[15:11]`, then writes unpacked
`Y,U,V` triplets. This reproduces the words presented to the historical type
19, Super Moving Blocks compressor. It is explicitly **not** a general
RGB555, YUV555, or 6Y5UV colour-space conversion, despite type 2 being able to
carry those labelled colour spaces.

No other Replay uncompressed format currently has a portable unpacker. The
AE7 reader can inspect their catalogue and chunk metadata but cannot decode
their pixels.

Type 23, `6Y6Y5U5V`, is the first additional packed format to implement. It is
4:2:2 YUV: each horizontal pair stores two independent six-bit Y samples and
one shared five-bit U/V pair. There is no vertical chroma subsampling. It
therefore uses 22 bits per pair, or 11 bits per pixel, and is lossless only
relative to that subsampled representation. Its compressor averages each
pair's signed-five-bit chroma when converting from per-pixel 6Y5UV.

Type 23 supplies the Moving Blocks family's native component precision without
the ambiguous type 2 reinterpretation. Types 8 and 21 are also useful candidates
because 24-bit RGB/YUV and packed YUYV have close FFmpeg pixel-format
equivalents. The exact Replay packing and signed chroma conventions must still
be verified before treating an FFmpeg format as byte-compatible.

An observed Acorn-tooling risk should remain recorded: selecting the colour
space labelled `6YVUV` in the original BASIC/ARM compressor can produce an
unplayable movie. The precise cause is not yet established. Portable tools
must use explicit pixel-layout names and test generated output in Replay,
rather than assuming that a historical colour-space label implies a valid
conversion path.

## Moving Blocks Family

The Moving Blocks-family types are exactly:

| Type | Name | Pixel precision | Main distinguishing feature |
| ---: | --- | --- | --- |
| 7 | Moving Blocks | 5Y5UV | original block syntax and fixed literal data blocks |
| 17 | Moving Blocks HQ | 5Y5UV | later 4x4/2x2 syntax and 32-symbol luma Huffman coding |
| 19 | Super Moving Blocks | 6Y5UV | 64-symbol luma Huffman coding with literal 5-bit chroma |
| 20 | Moving Blocks Beta | 6Y6UV | format-19-like luma with stateful delta-coded chroma |

Format 19 remains the recommended first encoder target. Format 17 is the
closest simpler-precision relative; format 20 adds chroma predictor state; and
format 7 is an older, less representative syntax.

## Non-Acorn Allocations

`AE7doc` reserves ranges rather than defining their bitstreams:

- 100-199: Eidos;
- 200-299: Irlam Instruments;
- 300-399: Wild Vision;
- 400-499: Aspex Software;
- 500-599: Iota;
- 600-699: Warm Silence Software;
- 800-899: small users, including Pederson formats 800 and 802;
- 900-999: Innovative Media Solutions.

The 2003 tree contains many imported AVI/QuickTime decompressors in these
ranges. They are playback compatibility components, not targets for the first
Replay encoder.

## Project Priority

For encoding modern video to Replay:

1. finish and verify format 19, Super Moving Blocks;
2. accept modern input through FFmpeg, initially as a documented raw pipe;
3. add type 23, 6Y6Y5U5V, as the first native packed 4:2:2 reader/writer;
4. add type 7, Moving Blocks, and type 17, Moving Blocks HQ;
5. add format 20, Moving Blocks Beta, then format 1, Moving Lines;
6. add other uncompressed formats where FFmpeg interoperability or container
   diagnostics justify them.

H.263 remains parked because its algorithm and existing tooling are separately
documented.
