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
| 102 (`Decomp102`) | Escape | © Eidos plc 1993 |
| 800 (`Decomp800`) | LinePack | © Henrik Bjerregaard Pedersen, 1995 |
| 802 (`Decomp802`) | Movie 16:3 | © Henrik Bjerregaard Pedersen, 1995 |

Escape (100/102) declares no colour model in its `Info`; pass `--video-colour`
to the transcoder for it. Decomp102 was included with the Computer Concepts
Eagle M2 video capture card, which can record in Escape 102 format.

### Note: Escape on later RISC OS Players

Escape (100 and 102) plays on the RISC OS 3.71 `!ARMovie.Player` but not on the
2003 one, because of a change to how the Player reads the codec's `Info` file.

- The **3.71 Player** reads `Info` lines 4 and 5 but ignores them, and calls the
  decompressor with the movie's exact width/height (no rounding).
- The **2003 Player** treats lines 4 and 5 as block-size triples
  `block;block;max` and derives a rounding mask `xround = (2nd field) - 1`
  (likewise `yround` from line 5). It then rounds the frame size **up** to that
  block and hands the rounded width to the decompressor:
  `xround = (sx + xround) AND NOT xround`.

Escape's 1993-era `Info` uses lines 4/5 for *maximum dimensions*
(`160;160;160` / `128;128;128`), not block triples, so the 2003 Player computes
`xround = 159`, `yround = 127`. A 160-wide frame is then rounded to 288 and the
decoder is told the frame is 288×128 — corrupting the decode/paint buffers.
(Modern codecs use e.g. `4;4;1280` → `xround = 3`, rounding width up to 4.)

This is a Player-side `Info`-format incompatibility, not a fault in the module:
it initialises and decodes correctly under this project's harness, which passes
the exact dimensions like the 3.71 Player.

#### Correct replacement `Info` for Escape

Escape needs no block rounding (the 3.71 Player ran it with exact dimensions), so
the fix is to make `xround`/`yround` evaluate to 0 — block size 1 — by setting
the second field of lines 4 and 5 to `1`. Only lines 4 and 5 change:

```text
Escape                 (1: name)
© Eidos plc 1993       (2: originator)
                       (3: blank -> 16bpp, old non-C calling sequence; correct)
1;1;160                (4: was 160;160;160  -> xround = 0, no width rounding)
1;1;128                (5: was 128;128;128  -> yround = 0, no height rounding)
Temporal,Spatial       (6: capabilities; correct as-is)
```

(The third field is the maximum dimension and is not used for rounding.) The
`Info` files vendored here are left **unmodified** as the authentic codec
fixtures; the above is the edit needed for the 2003 RISC OS Player.

## Source and acknowledgement

These modules are taken unmodified from a compiled RISC OS 2003 ARMovie build.
The Replay / ARMovie sources are maintained by **RISC OS Open Ltd**
(<https://gitlab.riscosopen.org/>, `RiscOS/Sources/SystemRes/ARMovie`); thanks to
RISC OS Open and the original Acorn and third-party authors.

Out of scope (not vendored): the MovieFS (Warm Silence Software, 600–699) and
VideoFS (Innovative Media Solutions, 900–999) codecs wrap Windows-world formats
(Cinepak, Indeo, …); and Iota's LZW (500) ships as an application rather than a
CodecIf module. ffmpeg's own Cinepak/Indeo decoders are the route for those.
