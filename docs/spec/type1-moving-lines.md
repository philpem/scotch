# Compression type 1 — Moving Lines

> **Status: verified.** This project has a portable Moving Lines codec
> (`src/codec_movinglines.c`) — a full decoder and an encoder — and it is
> cross-checked **byte-for-byte against the compiled Acorn `MovingLine` module**
> under emulation (`tools/movinglines_unicorn.py`, `test_movinglines_compiled`),
> over fixtures exercising every command family: literals, repeats, temporal and
> spatial copies, the same-position copy and the bit-packed long-literal run. So
> like types 7/17/19/20, the format below is machine-proven against Acorn's own
> decoder, not just read from the source.

Conventions (LSB-first bit order) are in [methodology.md](methodology.md).

## 1. Identity

`MovingLine/Resources/Info`: `Moving Lines`, `© Acorn Computers 1993`,
`Temporal,Spatial`, `YUV 5,5,5; RGB 5,5,5`. Compression type **1** — the oldest
Replay inter-frame codec.

- **Pixels are 15-bit**, either YUV555 or RGB555 per the movie's declared colour;
  the codec treats the 15 bits opaquely and the display tables interpret them.
- **Line-oriented, not block-oriented:** the unit is a run of pixels along the
  raster, not a 2-D block (its `Info` "block" is 1×1). Decoding walks the output
  in raster order, each command advancing the output pointer by the pixels it
  produces.
- Copies are **temporal** (from the previous reconstructed frame) or **spatial**
  (from the current frame), so the key/inter-frame model matches the Moving
  Blocks family: a key frame uses literals, runs and spatial copies only.

## 2. Stream unit

The stream is a sequence of **16-bit halfwords**; the source pointer must be
halfword-aligned. Each halfword is split:

```
flag    = word & 1
payload = (word >> 1) & 0x7FFF      (15 bits)
```

- `flag == 0` → the halfword is **one literal 15-bit pixel** (`payload`).
- `flag == 1` → the halfword is a **command** (§3).

## 3. Commands

Commands reuse the 15-bit payload in two different splittings. The decoder
selects by the command's high bits, dispatched in this order:

```
code = word >> 7                    (bits 7..15, 9 bits)
copy/repeat families use:  length = ((word >> 1) & 0x3F) + 2   (bits 1..6)
the 0x1E/0x1F families use: length = ((word >> 1) & 0x3FF) + 1 (bits 1..10)
```

| code (bits 7–15) | Family | Length field | Meaning |
| --- | --- | --- | --- |
| `0x000`–`0x11F` | temporal copy | bits 1–6, +2 (2..65) | copy `length` pixels from the **previous** frame at the offset-table entry for `code` |
| `0x120`–`0x1CB` | spatial copy | bits 1–6, +2 (2..65) | copy `length` pixels from the **current** frame at the offset-table entry for `code` |
| `0x1CC`, length≠0 | repeated pixel | bits 1–6, +2 (3..65) | followed by one 15-bit pixel halfword; emit it `length` times |
| `0x1CC`, length=0 | **end of frame** | — | `word == 1 + (0x1CC << 7)` (0xE601); the frame is complete |
| bits 11–15 = `0x1E` | same-position copy | bits 1–10, +1 (1..1024) | copy `length` pixels from the **previous** frame at the *same* output position |
| bits 11–15 = `0x1F` | long literal run | bits 1–10, +1 (1..1024) | followed by `length` packed 15-bit pixels (§3.2) |

The `0x1E`/`0x1F` families occupy `code` ≥ `0x1E0`, so they never collide with the
copy/repeat ranges; a decoder distinguishes them by testing bits 11–15 once `code`
is at or above `0x1CD`. Codes `0x1CD`–`0x1DF` are tolerated decoder aliases of the
repeat family; a conforming encoder emits repeats only as `0x1CC`.

### 3.1 Copies and the offset table

A copy's `code` indexes one offset table (the decompressor's `r5`), built by the
compressor and reproduced by the decoder's init:

- **Temporal offsets** (`code` `0x000`–`0x11F`) come from a ±8 search (a 17×17
  area, `move% = 8`) **with the centre entry omitted** — the same-position
  previous-frame copy is handled by the `0x1E` family instead.
- **Spatial offsets** (`code` `0x120`–`0x1CB`) come from `smove% = 9` but only for
  rows **above** the current row (`for j = −9 to −1; for i = −9 to +9`), so a
  spatial source is always already reconstructed. `smove% = 9` generates 171
  offsets, covering `0x120`–`0x1CA`; `0x1CB` is reserved/unused in this pass.

For a temporal copy the source is `current_output + offset`, then displaced by the
previous-frame offset; for a spatial copy it is `current_output + offset` in the
current buffer.

### 3.2 Long literal run packing

The `0x1F` command is followed by `length` **bit-packed** 15-bit pixels (not one
halfword each): the pixels are concatenated LSB-first into the byte stream. After
the packed pixels the stream is **realigned to the next 16-bit boundary** before
the next command. (Short literal runs — `length ≤ 15` — are written as individual
literal halfwords instead, §2.)

## 4. Frame structure

A frame is a sequence of commands consumed in output raster order until the
end-of-frame halfword (`0xE601`). There is no per-frame header; the decoder knows
the frame width/height from the container. Inter frames reference the previous
reconstruction (temporal and same-position copies); a key frame is built from
literals, repeated-pixel runs and spatial copies only. On frame 0 the compressor
lowers the run threshold (`rle% = 2`, spatial-only), since temporal copying is
unavailable.

The decompressor is generated in three output widths (`MakeDecomp PROCass(n)`):
4 bytes/pixel (`Decompress`), 2 (`DecompresH`) and 1 (`DecompresB`) per output
pixel. These differ only in how decoded pixels are stored; the **bitstream is
identical** across all three.

## Appendix A. Provenance and corrections

Sources (RISC OS 2003 tree; see [methodology.md](methodology.md) for the link
convention):
[`Video/MovingLine`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/MovingLine)
— `Resources/Info`, and the tokenised-BASIC `bas/BatchComp,ffb` (compressor) and
`bas/MakeDecomp,ffb` (decompressor generator). This spec also draws on the
project notes [`moving-lines-bitstream-first-pass`](../../notes/moving-lines-bitstream-first-pass.md),
[`moving-lines-decompressor`](../../notes/moving-lines-decompressor.md) and
[`moving-lines-compressor-process`](../../notes/moving-lines-compressor-process.md).

- **Cross-checked against the compiled module.** `codec_movinglines.c` is run
  against the genuine `MovingLine` decompressor under emulation for fixtures
  covering every command family, and the two agree byte-for-byte (the offset
  tables of §3.1 are thereby confirmed). The reference encoder emits a valid,
  round-trip-exact subset (same-position copies, repeats and literals); it does
  not search temporal/spatial offsets, so it is not bitrate-comparable to Acorn's
  compressor.
- **The long-literal pixels are read with word-aligned LDM, not unaligned.** The
  module loads each group of bit-packed pixels with `LDMIA` from the byte address
  `r0 >> 3` and then shifts by `r0 & 31` to reach the bit position; on Acorn ARM
  the LDM ignores the low address bits. This is a decoder detail, not a wire-
  format one — the packing in §3.2 is plain LSB-first 15-bit pixels — but it is
  what the emulation harness must reproduce (and was the one place the cross-check
  initially diverged before the alignment was emulated).
- **"Skip" is a same-position copy.** The compressor comment calls the `0x1E`
  family "skip n+1 pixels", which reads as "leave output unchanged"; the
  decompressor actually copies those pixels from the previous frame at the same
  position. For a static region the result is visually a skip, but a decoder must
  copy, not leave stale data.
- **The centre temporal offset is intentionally absent.** The ±8 temporal table
  omits the (0,0) entry; same-position previous-frame copies are the `0x1E`
  family, which carries a much longer (10-bit) length than the 6-bit copy
  families. An encoder that re-adds a centre offset to the copy table would
  diverge from Acorn's stream.
- **Pixel bit layout is colour-model-dependent.** The 15-bit pixel is YUV555 or
  RGB555 per the declared colour; the codec does not interpret it, so the
  field layout follows the colour model (as for type 2), not Moving Lines itself.
