# Methodology and shared conventions

This document underpins every other spec in this directory: the sources they are
built from, how their claims were verified, and the cross-cutting conventions
(bit order, colour model, terminology) they all assume. Read it once; the format
specs do not repeat it.

## Sources

The authority for every claim is **Acorn's own code** — the programs that
actually encoded and played the movies. This repository's reference
implementation is a cross-check, never the source of truth.

- **Shipped binaries and player source — authoritative for behaviour.** The
  compiled `Decompress,ffd` codec modules (`Decomp7`, `Decomp17`, `Decomp19`,
  `Decomp20`, `Decomp20new`, `Decomp23`, ...) and the ARMovie player itself:
  `RiscOS/Sources/SystemRes/ARMovie` (the engine, whose `bas/Player` BBC BASIC
  program assembles the playback loop with `[OPT`) and its `!ARPlayer` GUI
  front-end. For any disagreement **the shipped Acorn code wins**, and the
  container's hard constraints come from how this player behaves, bugs included
  (see the format specs' "player constraints" sections and
  [`../player-bugs.md`](../player-bugs.md)).
- **Acorn source and documentation — authoritative for intent and constants.**
  The BASIC compressor/decompressor listings (`BatchComp,ffb`, `MakeDecomp,ffb`,
  CompLib), per-codec `Resources/Info` identity files, and the occasional
  `Docs/Stream` note. These supply the algorithms and exact constants but contain
  transcription errors and omissions and sometimes describe a *different revision*
  from the one that shipped.
- **This repository's `libreplay` — a cross-check, not an authority.** A portable
  re-implementation built from the Acorn sources above and validated against the
  shipped binaries byte-for-byte. The specs cite Acorn's code; where `libreplay`
  and Acorn disagree, Acorn is correct.

When a spec states a fact, it cites the Acorn source it came from, and "verified"
means the behaviour was reproduced from the shipped Acorn binary (§Verification).

### The Acorn source tree

All Acorn citations are to the RISC OS 2003 source release at
**[github.com/barryc-ro/RiscOS_2003](https://github.com/barryc-ro/RiscOS_2003)**
(examined at commit `b40b11b`). Paths below are relative to that repository; a
file `X` links as `https://github.com/barryc-ro/RiscOS_2003/blob/master/X`. The
components the specs draw on:

| Component | Path | Contents |
| --- | --- | --- |
| ARMovie player engine | [`RiscOS/Sources/SystemRes/ARMovie`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie) | playback loop ([`bas/Player,ffb`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/SystemRes/ARMovie/bas/Player%2Cffb)), colour/format library ([`bas/CompLib,ffb`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/SystemRes/ARMovie/bas/CompLib%2Cffb)) |
| Per-codec modules | `RiscOS/Sources/SystemRes/ARMovie/Video/Decomp`*N* | e.g. Decomp19's `Resources/Info`, `bas/BatchComp,ffb`, `bas/MakeDecomp,ffb` |
| GUI front-end | [`RiscOS/Sources/Apps/ARPlayer`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/Apps/ARPlayer) | poster handling (the missing-sprite crash) |
| Encoder app | [`RiscOS/Sources/Apps/AREncode`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/Apps/AREncode) | the GUI encoder |
| Replay libraries | [`RiscOS/Sources/Lib/ARLib`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/Lib/ARLib) | shared Replay code (sprite, file, struct helpers) |

The `,ffb` files are tokenised BBC BASIC; the line numbers some specs quote are
program line numbers, visible after detokenising (see
[`../player-bugs.md`](../player-bugs.md) for the procedure).

## Verification method

The behaviour the specs describe is read out of the **genuine Acorn decoder**,
not assumed from the reference code:

1. **Emulated execution of the genuine Acorn decoder.** `tools/decomp19_unicorn.py`
   loads a real `Decompress,ffd` into a Unicorn ARM emulator, installs the
   classic-alignment LDM/STM hooks the modules rely on, and runs the codec over
   chosen payloads — initialising once and alternating reconstruction buffers so
   inter-frame dependencies are exercised exactly as in playback. See
   [`../decomp19-arm-harness.md`](../decomp19-arm-harness.md). Container and
   playback constraints are read from the player engine's `bas/Player` BASIC
   (detokenised) and `!ARPlayer`.
2. **Byte-for-byte cross-checks.** The reference codec (`libreplay`) is held to
   the Acorn decoder's output, frame by frame, for hand-built fixtures and for
   multi-frame synthetic movies (`test_fullmovie_*`). The corpus is described in
   [`../cross-check-corpus.md`](../cross-check-corpus.md). These tests `SKIP` when
   the compiled modules or Unicorn are unavailable.

A claim is "verified" when it reproduces the shipped Acorn binary's behaviour
byte-for-byte. The reference codec is how that check is run; it is not what the
spec cites.

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
the optional ordered-dither luma rounding come from the ARMovie colour library
`bas/CompLib,ffb`; a future colour spec will lift them here.

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
