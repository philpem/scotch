# Methodology and shared conventions

This document underpins every other spec in this directory: the sources they are
built from, how their claims were verified, and the cross-cutting conventions
(bit order, colour model, terminology) they all assume. Read it once; the format
specs do not repeat it.

## Sources

Two kinds of original material exist, and they are not equally trustworthy.

- **Shipped, compiled decoders** — `Decompress,ffd` modules from the ARMovie /
  Replay distributions (`Decomp7`, `Decomp17`, `Decomp19`, `Decomp20`,
  `Decomp20new`, `Decomp23`, ...). These are the programs that actually played
  the movies, so for any disagreement **the running decoder is authoritative.**
- **Source and documentation** — per-codec `Resources/Info` identity files,
  occasional `Docs/Stream` notes, and Acorn's BASIC compressor/decompressor
  listings (`BatchComp,ffb`, `MakeDecomp,ffb`, CompLib). These explain intent
  and supply constants, but they contain transcription errors and omissions and
  sometimes describe a *different revision* from the one that shipped.

When a spec states a fact, it states it as verified against the compiled
decoder. When it quotes the BASIC or the `Info` file, it says so.

## Verification method

Three independent checks back the specs:

1. **A portable reference codec** (this repository's `libreplay`) encodes and
   decodes each format.
2. **Emulated execution of the genuine Acorn decoder.** `tools/decomp19_unicorn.py`
   loads a real `Decompress,ffd` into a Unicorn ARM emulator, installs the
   classic-alignment LDM/STM hooks the modules rely on, and runs the codec over
   chosen payloads — initialising once and alternating reconstruction buffers so
   inter-frame dependencies are exercised exactly as in playback. See
   [`../decomp19-arm-harness.md`](../decomp19-arm-harness.md).
3. **Byte-for-byte cross-checks.** The reference decoder's output is compared
   against the emulated Acorn decoder's output, frame by frame, for hand-built
   fixtures and for multi-frame synthetic movies (`test_fullmovie_*`). The
   corpus is described in [`../cross-check-corpus.md`](../cross-check-corpus.md).
   These tests `SKIP` when the compiled modules or Unicorn are unavailable.

A claim is "verified" in a spec when the reference implementation that embodies
it reproduces the compiled Acorn decoder's output byte-for-byte.

## Conventions

### Bit order (important)

Replay bitstreams are **little-endian at the bit level**: within a byte the
first transmitted bit is bit 0 (the least-significant bit), and a multi-bit
field is filled least-significant bit first. A decoder reads a field by taking
the next *N* buffered bits as an integer whose bit 0 is the earliest bit on the
wire.

Throughout these specs a field's **value is that integer**, never the order the
bits happen to appear on the wire. This matters because the two notations are
bit-reverses of each other. For example the type-19 4×4 opcode is a 2-bit field:

| Value | Bits on the wire (first, second) | Meaning |
| --- | --- | --- |
| 0 | 0, 0 | stationary |
| 1 | 1, 0 | data |
| 2 | 0, 1 | move (temporal/spatial) |
| 3 | 1, 1 | split |

Some Acorn docs and early notes write these opcodes in *transmission order*
(`00`, `10`, `01`, `11`), which makes "data" look like `10` and "move" like
`01`. That is the same format described the other way round; this spec always
gives the integer value.

A field of width 0 is absent (no bits). The final byte of a payload is padded
with zero in its unused high bits; conforming decoders require that padding to
be zero and that no whole trailing bytes remain.

### Colour: the 6Y*n*UV working model

The Moving Blocks family works in a luma/chroma space, never RGB, internally:

- **Luma (Y)** is an unsigned fixed-bit value: 6-bit (0..63) for 6Y5UV/6Y6UV,
  5-bit (0..31) for the YUV555 used by type 17.
- **Chroma (U, V)** are **signed** values stored modulo 2·*half*: a stored value
  `s` denotes `s` when `s < half`, else `s - 2·half`. *half* is 16 for 5-bit
  chroma (range −16..15) and 32 for 6-bit chroma (range −32..31).

A 16-bit packed 6Y5UV sample places Y in bits 0–5, U in bits 6–10 and V in bits
11–15.

RGB↔YUV uses the BT.601 luma weights in 16.16 fixed point (0.299, 0.587, 0.114
→ 19595, 38470, 7471, summing to 65536), with U and V derived from B−Y and R−Y.
All integer rounding emulates the ARM arithmetic shift (round toward negative
infinity), which differs from C's truncate-toward-zero for negatives; encoders
must reproduce this to match Acorn output bit-for-bit. The exact constants and
the optional ordered-dither luma rounding are given in the reference
`mb_color.c`; a future colour spec will lift them here.

### Terminology

- **Key frame / inter frame.** A key frame is independently decodable (it uses
  only data and intra-frame *spatial* copies). An inter frame may additionally
  copy from the previous reconstructed frame (*stationary* and *temporal*). In
  MPEG terms these are I-like and P-like; there is no future reference (no
  B-like frame).
- **Reconstruction.** The decoder-visible pixels a frame produces. The next
  inter frame references this reconstruction, never the encoder's source — an
  encoder that references its source instead will desync after the first lossy
  or chroma-averaged block.
