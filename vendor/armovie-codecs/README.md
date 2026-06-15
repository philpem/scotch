# Acorn Replay (ARMovie) decompressor modules

The original Replay / ARMovie compiled video `Decompress` modules, run unpatched
under the vendored ARMulator (see `../armulator`) so they emit each codec's
native working-colour words. They serve two purposes here: **byte-exact
cross-check oracles** for the portable C codecs, and the actual decoders used by
`replay-transcode` to turn Replay movies into video.

Each codec directory holds the compiled ARM module `Decompress,ffd` (RISC OS
filetype `&FFD`) and its `Info` file (the codec's working-colour line is read
from `Info` for codecs not built into the transcoder's table).

## Acorn codecs

Distributed by Acorn as **freeware**, now **open source via RISC OS Open Ltd**.

| Codec | Format | Codec | Format |
|------:|--------|------:|--------|
| 1 (`MovingLine`) | Moving Lines | 11 | 16Y1UV8 (9bpp) |
| 2  | 16bpp uncompressed | 16 | 4Y1UV8 (12bpp) |
| 3  | YYUV (10bpp) | 17 | Moving Blocks HQ |
| 4  | 8bpp uncompressed | 19 | Super Moving Blocks |
| 5  | 4Y1UV (8bpp) | 20 / `Decomp20new` | Moving Blocks Beta (+v0.05) |
| 6  | 16Y1UV (6bpp) | 21 | YUYV8 (16bpp) |
| 7  | Moving Blocks | 22 | YY8UVd4 (12bpp) |
| 8  | 24bpp uncompressed | 23 | 6Y6Y5U5V |
| 9  | YYUV8 (16bpp) | 24 | 4x6Y1x5UV (8.5bpp) |
| 10 | 4Y1UV8 (12bpp) | 25/26/27 | YYYY..UV.. subsampled |

## Non-Acorn codecs

Third-party Replay codecs, also distributed as **freeware**, included with
acknowledgement of their respective copyright holders:

| Codec | Format | Copyright |
|------:|--------|-----------|
| 100 (`Decomp100`) | Escape | © Eidos plc 1993 |
| 800 (`Decomp800`) | LinePack | © Henrik Bjerregaard Pedersen, 1995 |
| 802 (`Decomp802`) | Movie 16:3 | © Henrik Bjerregaard Pedersen, 1995 |

Escape (100) declares no colour model in its `Info`; pass `--video-colour` to the
transcoder for it.

## Source and acknowledgement

These modules are taken unmodified from a compiled RISC OS 2003 ARMovie build.
The Replay / ARMovie sources are maintained by **RISC OS Open Ltd**
(<https://gitlab.riscosopen.org/>, `RiscOS/Sources/SystemRes/ARMovie`); thanks to
RISC OS Open and the original Acorn and third-party authors.

Out of scope (not vendored): the MovieFS (Warm Silence Software, 600–699) and
VideoFS (Innovative Media Solutions, 900–999) codecs wrap Windows-world formats
(Cinepak, Indeo, …); and Iota's LZW (500) ships as an application rather than a
CodecIf module. ffmpeg's own Cinepak/Indeo decoders are the route for those.
