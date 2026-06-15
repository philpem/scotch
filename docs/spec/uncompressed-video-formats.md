# Replay uncompressed video formats

Replay/ARMovie video format numbers 2–25 include a family of **uncompressed**
(fixed-bitrate, directly-packed) formats alongside the entropy-compressed ones
(1, 7, 17, 18, 19, 20). This note documents the uncompressed formats and how
`replay-transcode` decodes them.

Sources: `ARMovie_2003/Resources/Documents/AE7doc` (format catalogue and colour
models), each codec's `Info` file (the authoritative working-output colour
line), and the `Video/Decomp<N>/bas/MakeDecomp` generators (bit packing). See
also [type1-moving-lines.md](type1-moving-lines.md) and
[ae7-armovie-container.md](ae7-armovie-container.md).

## How decoding works

Every Replay video format — compressed or not — ships a `Decompress` module
following the CodecIf contract (three-word header: patch-table offset, init
branch, decompress branch). The uncompressed formats' modules are tiny (a few
hundred bytes) because they only unpack fixed-layout pixels; the compressed
ones are larger. The CodecIf rule that makes a generic decoder possible is
stated in every `MakeDecomp`:

> *"Note that an unpatched decompressor should still work!!"* … *"An unpatched
> format 2 decompressor produces RGB or YUV output."*

The colour-lookup **patch table** would normally be filled in by the player to
fold a screen-format colour map into the decoder. Left unpatched, the decoder
writes one **working-colour word per pixel** instead. `replay-transcode` runs
the module unpatched (via `replay/codecif.h`, under the vendored ARMulator) and
reads those words, so it needs no per-format bit-unpacking of its own — the
Acorn module is the authoritative unpacker. The only per-format knowledge the
transcoder needs is **how to interpret the output word**, which is given by the
codec's `Info` file colour line (line 7).

Type 23 additionally has a native C unpacker (`replay_type23_*`) used directly,
since its layout is simple and self-contained; it does not need the module.

## Working-output word layouts

The unpatched output word packs components low-to-high. Confirmed from the
generators (e.g. Decomp16 builds `00 vv uu YY`, i.e. Y in the low byte):

| Colour line (Info) | Word layout (bit ranges) | Transcoder handling |
|---|---|---|
| `YUV 5,5,5`     | Y[0:4] U[5:9] V[10:14]            | YUV555 → RGB (CompLib) |
| `RGB 5,5,5`     | R[0:4] G[5:9] B[10:14] (red low)  | RGB555 → RGB |
| `YUV 6,5,5` / `6Y5UV 6,5,5` | Y[0:5] U[6:10] V[11:15] | 6Y5UV → RGB (CompLib) |
| `6Y6UV 6,6,6`   | Y[0:5] U[6:11] V[12:17]           | 6Y6UV → RGB (CompLib) |
| `YUV 8,8,8`     | Y[0:7] U[8:15] V[16:23]           | YUV888 → RGB (CCIR, best-effort) |
| `RGB 8,8,8`     | R[0:7] G[8:15] B[16:23]           | RGB888 → RGB |
| `8`             | 8-bit index/greyscale             | palette (header `palette <off>`) or greyscale |

U/V in the YUV models are signed (CCIR-scaled) chroma; the 5/6-bit forms use the
modulo representation described in `mb_frame.h`.

## Format catalogue

Depth and colour from each `Decomp<N>/Info`; descriptions from AE7doc. "uses"
in AE7doc refers to the chroma-sharing pattern, not the output word — the output
word is the Info colour line.

| Fmt | Name | bpp | Output colour | Notes |
|----:|------|----:|---------------|-------|
| 2  | 16 bit colour uncompressed | 16  | YUV/RGB/6Y5UV per header | model from the movie's `[...]` colour label; default RGB |
| 3  | YYUV uncompressed          | 10  | YUV 5,5,5 | chroma horiz ÷2 |
| 4  | 8 bit uncompressed         | 8   | 8-bit | palette (256×RGB at header `palette <off>`) or greyscale; pixel depth = 8 |
| 5  | 4Y1UV uncompressed         | 8   | YUV 5,5,5 | chroma ÷2 horiz & vert |
| 6  | 16Y1UV uncompressed        | 6   | YUV 5,5,5 | chroma ÷4 horiz & vert |
| 8  | 24 bit colour uncompressed | 24  | YUV/RGB 8,8,8 per header | model from header; default RGB |
| 9  | YYUV8 uncompressed         | 16  | YUV 8,8,8 | chroma horiz ÷2 |
| 10 | 4Y1UV8 uncompressed        | 12  | YUV 8,8,8 | chroma ÷2 horiz & vert |
| 11 | 16Y1UV8 uncompressed       | 9   | YUV 8,8,8 | chroma ÷4 horiz & vert |
| 16 | 4Y1UV8 uncompressed        | 12  | YUV 8,8,8 | chroma ÷2 horiz & vert |
| 21 | YUYV8 uncompressed         | 16  | YUV 8,8,8 | chroma horiz ÷2 |
| 22 | YY8UVd4                    | 12  | YUV 8,8,8 | chroma horiz ÷2; spatially predicted |
| 23 | 6Y6Y5U5V uncompressed      | 11  | 6Y5UV (YUV 6,5,5) | chroma horiz ÷2; native unpacker `replay_type23` |
| 24 | 4x6Y1x5UV uncompressed     | 8.5 | 6Y5UV (YUV 6,5,5) | chroma ÷2 horiz & vert |
| 25 | YYYYd4UVd4                 | 6   | 6Y6UV (YUV 6,6,6) | chroma ÷2 horiz & vert |

Not decoded: 12/13 (MPEG), 14 (Ultimotion), 15 (named/indirect video) — these
need external codecs.

## Unknown codecs: driven by the Info file

For a video format the transcoder has no built-in entry for, it falls back to a
generic path: it locates the decompressor (`--module FILE`, or
`<modules-dir>/Decomp<N>/Decompress,ffd`), reads the working-output colour model
from line 7 of the codec's `Info` file (the colour line above), runs the module
unpatched under codecif, and converts accordingly. So an arbitrary external
decompressor "just works" given its `Decompress` code and `Info` file. The
built-in table above is used for the known formats (and matches their Info).

## Status / caveats

- The decode (module → working words) is authoritative for every format: it is
  Acorn's own `Decompress` code run unpatched.
- RGB output is exact for the 6Y5UV/6Y6UV/YUV555 families (the CompLib preview
  converters this project already uses) and for the straight RGB layouts.
- **YUV 8,8,8 → RGB uses a standard CCIR matrix and is best-effort**: no
  8-bit-YUV sample movies were available to verify the exact chroma scaling.
- Format 4 palette lookup uses the header `palette <offset>` table when present,
  otherwise greyscale.
