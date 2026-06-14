# Compression type 19 — Super Moving Blocks

Authoritative bitstream specification. Assumes the conventions in
[methodology.md](methodology.md), especially **least-significant-bit-first** bit
order: every field value below is the integer a decoder accumulates from the
next *N* bits, bit 0 first.

## 1. Identity

`Decomp19/Resources/Info` declares:

```text
Super Moving Blocks
Acorn Computers 1996
,C
4;4;1280
4;4;1024
Temporal,Spatial
6Y5UV 6,5,5
```

- **Codec number:** 19.
- **Working colour:** 6Y5UV — 6-bit luma (0..63), signed 5-bit U and V
  (modulo 32, range −16..15; see methodology §Colour). A 16-bit packed sample is
  `Y | (U << 6) | (V << 11)`.
- **Block grid:** 4×4. Frame width and height must be multiples of 4.
- **Copy classes offered:** Temporal and Spatial (plus stationary, which is the
  zero-vector temporal special case).

Type 19 shares its block grammar, motion tables and luma Huffman table with type
17 (Moving Blocks HQ); it differs only in carrying 6-bit rather than 5-bit luma.
The shipped compressor identifies itself as *"format 19 Version 0.04, 6th
September 1996"*.

## 2. Frame structure

A frame payload is a sequence of **4×4 block records in raster order**: left to
right across each row of blocks, top to bottom down the frame. There is no frame
header and no per-block length prefix — block boundaries are implicit in the
grammar. Decoding ends when the last 4×4 block of the bottom-right has been read;
the payload is then byte-aligned with zero padding (§8).

Each 4×4 block begins with a **2-bit opcode**:

| Opcode | Block kind | References |
| --- | --- | --- |
| 0 | Stationary 4×4 | previous frame, same position |
| 1 | Data 4×4 | none (intra) |
| 2 | Move 4×4 | previous frame (temporal) or current reconstruction (spatial) |
| 3 | Split | four 2×2 sub-blocks |

A **key frame** (the first frame, or any frame decoded without a previous
reference) must use only opcodes 1 (data) and 2 with a *spatial* vector. An
**inter frame** may additionally use opcode 0 (stationary) and opcode 2 with a
*temporal* vector.

### 2.1 Stationary 4×4 (opcode 0)

No further fields. The block is copied verbatim from the previous reconstructed
frame at the same (x, y).

### 2.2 Move 4×4 (opcode 2)

The opcode is followed by a **motion code** (§4) selecting a vector
`(dx, dy)` and whether it is temporal or spatial. The 4×4 block is copied from:

- the **previous reconstructed frame** at `(x+dx, y+dy)` for a temporal vector;
- the **current frame's reconstruction so far** at `(x+dx, y+dy)` for a spatial
  vector.

Spatial vectors point only up and to the left (§4.3), so the source pixels are
already reconstructed. The reference rectangle must lie wholly inside the frame.

### 2.3 Split (opcode 3)

The 4×4 is divided into four 2×2 sub-blocks, decoded in the order **TL, TR, BL,
BR** (top-left, top-right, bottom-left, bottom-right). Each sub-block is one 2×2
record (§3). Splitting lets an encoder spend bits only on the parts of a block
that need them.

### 2.4 Data 4×4 (opcode 1)

A data block is the only intra carrier of new pixels. Its layout is:

```
opcode : 2     (value 1)
U      : 5     signed 5-bit block-average chroma
V      : 5     signed 5-bit block-average chroma
luma   : 16 Huffman-coded residuals, raster order within the 4×4
```

The 2-bit opcode and the two 5-bit chroma fields form a single **12-bit header**
(`opcode | (U << 2) | (V << 7)`). The whole 4×4 reconstructs to one (U, V) pair;
luma is per-pixel and lossless (§5–§6).

## 3. 2×2 sub-block records (inside a split)

A 2×2 record uses a shorter opcode than a 4×4 (it never splits, and its move
prefix is 1 bit):

| Leading bits (value) | Sub-block kind |
| --- | --- |
| `1` (1) | Move 2×2 — followed by a motion code (§4) |
| `00` | Stationary 2×2 |
| `01…` | Data 2×2 — the `01` is the low two bits of the 12-bit data header |

Read one bit. If it is 1, a motion code follows (move). If it is 0, read a second
bit: 0 ⇒ stationary; 1 ⇒ data, in which case the two bits just read are the low
two bits of a 12-bit data header (opcode value 2) and decoding rewinds to read
the full header.

- **Move 2×2** copies a 2×2 rectangle, temporally or spatially, exactly as §2.2
  but 2×2. A spatial 2×2 vector may reference an earlier sub-block of the *same*
  parent (in TL,TR,BL,BR order) as well as earlier 4×4 blocks; it may not
  reference a sub-block not yet decoded.
- **Data 2×2** has the 12-bit header (opcode value 2, then U:5, V:5) followed by
  **4** Huffman luma residuals in raster order.

## 4. Motion codes

A motion code is read as a **2-bit family** then a variable-width **index**:

| Family | Index width | Meaning |
| --- | --- | --- |
| 0 | 3 | Temporal, radius-1 ring (8 vectors), `ring(1, index)` |
| 2 | 4 | Temporal, radius-2 ring (16 vectors), `ring(2, index)` |
| 1 | 5 | index 0–7: **spatial** table entry; index 8–31: temporal radius-3 ring `ring(3, index−8)` |
| 3 | 8 | Temporal `far` vector (240 vectors), `far(index)` |

Index growth tracks code length, so a copy of the shortest move family is the
cheapest. The total move cost is the block's move prefix (2 bits for a 4×4, 1
bit for a 2×2) plus 2 family bits plus the index.

The zero vector `(0, 0)` is **not** encoded as a motion code; it is the separate
stationary opcode.

### 4.1 The temporal ring `ring(r, i)`

`ring(r, ·)` enumerates the `8r` integer points at Chebyshev distance exactly `r`
(the perimeter of the (2r+1)² square), in this order:

1. **Top edge**, `dy = −r`, `dx = −r … +r` → indices `0 … 2r`.
2. **Side edges**, `dy = −r+1 … r−1`, interleaved: each row contributes the left
   column (`dx = −r`) at the even index then the right column (`dx = +r`) at the
   odd index.
3. **Bottom edge**, `dy = +r`, `dx = −r … +r`.

Radii 1, 2 and 3 are reached by families 0, 2 and 1 respectively.

### 4.2 The `far` family `far(i)`

`far(·)` enumerates every vector in the `[−8, +8]²` square **except** the inner
`[−3, +3]²` box (which the ring families already cover), in raster order: `dy`
from −8 to +8 (outer), `dx` from −8 to +8 (inner), skipping any point with
`|dx| ≤ 3` and `|dy| ≤ 3`. That is 17² − 7² = 240 vectors, indices 0–239.

### 4.3 The spatial tables

Spatial vectors reference already-reconstructed pixels of the current frame, so
they point up and/or left. The eight entries differ by block size:

```
spatial 4×4 (index 0..7):  (−2,−4) (−1,−4) (0,−4) (1,−4) (2,−4) (−4,0) (−4,−1) (−4,−2)
spatial 2×2 (index 0..7):  (−2,−2) (−1,−2) (0,−2) (1,−2) (2,−2) (−2,−1) (−2,0) (−3,0)
```

A spatial source rectangle must be wholly reconstructed: for a 4×4 it must lie
in a 4×4 block earlier in raster order; for a 2×2 split child it may also lie in
an earlier child of the same parent.

## 5. Chroma reconstruction

Every data block stores one signed 5-bit `U` and one `V` — the block's average
chroma — and reconstructs **all** of its pixels with that pair. The average is a
floor average (rounding toward −∞, matching the ARM arithmetic shift) over the
block's source chroma. Copy blocks carry no chroma; they inherit whatever their
source pixels hold.

## 6. Luma reconstruction

### 6.1 Predictor topology

Luma is coded as residuals against a spatial predictor. Within a **4×4** data
block, raster order, the prediction for pixel (row, col) is:

- **(0, 0):** the carried cross-block predictor (§6.2).
- **rest of row 0:** the pixel immediately to the left.
- **col 0 of rows 1–3:** the pixel immediately above.
- **interior:** `floor((left + above) / 2)`.

A **2×2** data block uses the same idea reduced to four pixels: TL predicts from
the carried predictor; TR from TL; BL from TL; BR from `floor((TR + BL) / 2)`.

### 6.2 Cross-block luma predictor

One luma predictor is carried across the frame, **updated only by data blocks**
(copy blocks leave it unchanged). It is **0 at the start of every frame**. After
a data block it becomes that block's average luma: `sum_of_block_luma >> 4` for a
4×4 (`>> 2` for a 2×2). The first pixel of the next data block predicts from this
value.

### 6.3 Residual coding

Each residual is `(luma − prediction) mod 64`, a 6-bit symbol (0..63). Symbols
0–31 are deltas 0..+31; symbols 32–63 are deltas −32..−1 (the value minus 64).
The decoder forms `luma = (prediction + symbol) mod 64`. Each symbol is
Huffman-coded with the table in §7.

## 7. Luma Huffman table

64 symbols, maximum length 11 bits. `code` is the value the LSB-first reader
accumulates while matching, and `len` its bit length. (This table is shared with
type 17; `replay-verify --codec 19 --verify-huffman` checks a decoder against
it.)

| sym | code | len | sym | code | len | sym | code | len | sym | code | len |
| ---:| ----:|----:| ---:| ----:|----:| ---:| ----:|----:| ---:| ----:|----:|
| 0 | 0x002 | 2 | 16 | 0x18c | 9 | 32 | 0x690 | 11 | 48 | 0x04c | 9 |
| 1 | 0x007 | 3 | 17 | 0x1d4 | 9 | 33 | 0x290 | 11 | 49 | 0x041 | 9 |
| 2 | 0x00d | 4 | 18 | 0x194 | 9 | 34 | 0x710 | 11 | 50 | 0x068 | 8 |
| 3 | 0x019 | 5 | 19 | 0x190 | 9 | 35 | 0x310 | 11 | 51 | 0x054 | 8 |
| 4 | 0x01c | 5 | 20 | 0x2a1 | 10 | 36 | 0x414 | 11 | 52 | 0x00c | 8 |
| 5 | 0x018 | 5 | 21 | 0x341 | 10 | 37 | 0x014 | 11 | 53 | 0x021 | 8 |
| 6 | 0x031 | 6 | 22 | 0x34c | 10 | 38 | 0x214 | 11 | 54 | 0x028 | 7 |
| 7 | 0x034 | 6 | 23 | 0x294 | 10 | 39 | 0x094 | 11 | 55 | 0x02c | 7 |
| 8 | 0x030 | 6 | 24 | 0x314 | 10 | 40 | 0x28c | 11 | 56 | 0x001 | 7 |
| 9 | 0x061 | 7 | 25 | 0x541 | 11 | 41 | 0x090 | 10 | 57 | 0x008 | 6 |
| 10 | 0x06c | 7 | 26 | 0x141 | 11 | 42 | 0x110 | 10 | 58 | 0x011 | 6 |
| 11 | 0x050 | 7 | 27 | 0x68c | 11 | 43 | 0x114 | 10 | 59 | 0x000 | 5 |
| 12 | 0x0c1 | 8 | 28 | 0x54c | 11 | 44 | 0x08c | 10 | 60 | 0x004 | 5 |
| 13 | 0x0cc | 8 | 29 | 0x14c | 11 | 45 | 0x0a1 | 10 | 61 | 0x009 | 5 |
| 14 | 0x0e8 | 8 | 30 | 0x494 | 11 | 46 | 0x010 | 9 | 62 | 0x005 | 4 |
| 15 | 0x1a1 | 9 | 31 | 0x614 | 11 | 47 | 0x0d4 | 9 | 63 | 0x003 | 3 |

The codes are a prefix-free set under LSB-first reading: no complete code is the
low-bit prefix of another. A decoder may match by accumulating bits one at a time
(symbol = the entry whose `len` equals the bits read and whose `code` equals the
accumulated value), or build a lookup table.

## 8. Termination and padding

A frame ends after its last 4×4 block. The encoder then pads the final byte with
zero in the unused high bits. A conforming decoder requires that the consumed bit
count rounds up to exactly the payload length and that the trailing padding bits
are zero; trailing whole bytes or non-zero padding are malformed. In the AE7
container the per-chunk video region may carry one extra padding byte after the
last frame (see the container spec).

## 9. Encoder notes (non-normative for decoding)

A decoder needs nothing in this section; an encoder does.

- **Copy acceptance.** Whether a copy may stand in for a block is governed by the
  shared 29-row "QP%" quality table: row 0 demands exact decoder-visible pixels,
  higher rows permit bounded per-component and total error (luma absolute error
  plus the block-average chroma error). The table and the exact acceptance test
  live in the reference `mb_quality.c`; a future encoder spec will lift them.
- **Family choice.** Because index width grows with code length, an encoder that
  wants the original compressor's output prefers, among equally good copies, the
  shortest family and then the lowest index.
- **Data vs split.** When no copy is accepted, the encoder weighs a single data
  4×4 against a 2×2 split and keeps whichever is smaller (ties stay 4×4).
- **Determinism.** With copy modes and a fixed quality row, one frame encodes
  deterministically; whole-frame rate control (retrying at looser rows to hit a
  byte budget) is layered outside the core.

## Appendix A. Provenance and corrections

Sources: `Decomp19/Resources/Info`, `Decomp19/bas/BatchComp,ffb.txt`,
`Decomp19/bas/MakeDecomp,ffb.txt`, and the compiled `Decomp19/Decompress,ffd`.
There is **no `Docs/Stream` file** for Decomp19 in the reference tree, so the
wire format was recovered from the compressor source and confirmed against the
compiled decoder under emulation (methodology §Verification). Every frame of the
`LionFish19` sample and of the synthetic full-movie fixtures decodes identically
on the compiled module and the reference decoder.

Corrections and clarifications to the original material:

- **Opcode notation is bit-reversed in the source notes.** The BASIC and the
  early notes write 4×4 opcodes in transmission order (`00`/`10`/`01`/`11`),
  which lists "data" as `10` and "move" as `01`. Under the format's
  LSB-first reading those are the integer values 1 and 2 used here. Same format,
  opposite bit notation; this spec uses the integer value throughout.
- **The 2×2 spatial table is mis-ordered in the documentation.** Acorn's
  `Docs/Stream`-style listings give the 2×2 spatial column in a scrambled order.
  The order in §4.3 was read out of the running decoder (it is shared with types
  7 and 17, where it could be probed directly) and is the authority.
- **`Resources/Info` understates the dependency model.** "Temporal,Spatial" does
  not mention the stationary (zero-vector) opcode or that spatial copies make a
  frame self-contained; both were established from the grammar and confirmed by
  decoding spatial-only payloads with no previous frame.
- **Shipped revision vs source.** The Decomp19 source carries version "0.04".
  Unlike type 20 (which shipped in two incompatible chroma revisions), only one
  type-19 revision is known, and the compiled module matches the 0.04 source.
- **Colour rounding.** The `Info` file gives only "6Y5UV 6,5,5". The signed
  chroma modulus, the floor (ARM-ASR) averaging, and the BT.601-in-16.16
  forward constants were taken from CompLib and verified numerically; the exact
  real-to-integer rounding of CompLib's assembled constants at coefficient
  boundaries is reproduced by the reference but is the one detail still pending a
  from-assembler confirmation (it does not affect any cross-checked fixture).
