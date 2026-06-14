# Moving Blocks types 7, 17 and 20

Authoritative specification of the other three Moving Blocks codecs, written as
**deltas from [type 19](type19-super-moving-blocks.md)**. Type 19 is the
baseline; read it first. Conventions (LSB-first bit order, the 6Y*n*UV colour
model) are in [methodology.md](methodology.md).

All four codecs share: the 4×4 raster block scan; splitting a 4×4 into four 2×2
children in TL, TR, BL, BR order; the spatial reference tables (type 19 §4.3);
the key-frame / inter-frame model; and zero-padded byte alignment at end of
frame. What differs per codec:

| Type | Name | Colour | Luma coding | Motion coding | Module version |
| ---:| --- | --- | --- | --- | --- |
| 7 | Moving Blocks | YUV555 | literal 5-bit, no prediction | format 7 (±4) | 0.05, 22 Aug 1996 (© 1993) |
| 17 | Moving Blocks HQ | YUV555 | Huffman 5-bit residuals | format 19 (±8) | 0.03, 27 Aug 1996 |
| 19 | Super Moving Blocks | 6Y5UV | Huffman 6-bit residuals | format 19 (±8) | 0.04, 6 Sep 1996 |
| 20 | Moving Blocks Beta | 6Y6UV | Huffman 6-bit residuals (= 19) | format 19 (±8) | 0.05, 19 Nov 1996 |

---

## 1. Type 17 — Moving Blocks HQ

`Resources/Info`: `Moving Blocks HQ`, `© Acorn Computers 1996`, `YUV 5,5,5`,
`Temporal,Spatial`.

Type 17 is **type 19 with 5-bit luma instead of 6-bit.** Identical in every other
respect: the 2-bit 4×4 opcodes (0 stationary, 1 data, 2 move, 3 split), the 2×2
sub-block grammar, the 12-bit data header (`opcode | U<<2 | V<<7`, signed 5-bit
U/V), the motion-code families and tables (format 19, §4), the luma predictor
topology and the cross-block predictor, and termination. The only changes:

- **Colour is YUV555:** luma is 5-bit (0..31). U and V are the same signed 5-bit
  chroma as type 19 (`YUV 5,5,5`).
- **Luma residuals are modulo 32** (5-bit) and use the **32-symbol** Huffman
  table below — not type 19's 64-symbol table. Symbols 0–15 are deltas 0..+15,
  symbols 16–31 are deltas −16..−1; the decoder forms
  `luma = (prediction + symbol) mod 32`.

Luma is lossless: every residual has a code, so each reconstructed pixel equals
the source luma, and the predictor (§6 of type 19, evaluated on the decoded
neighbours) follows the decoder exactly.

### 1.1 Type 17 luma Huffman table

32 symbols, maximum length 9 bits. `code` is the value the LSB-first reader
accumulates; `len` its bit length.

| sym | code | len | sym | code | len | sym | code | len | sym | code | len |
| ---:| ----:|----:| ---:| ----:|----:| ---:| ----:|----:| ---:| ----:|----:|
| 0 | 0x002 | 2 | 8 | 0x065 | 7 | 16 | 0x16d | 9 | 24 | 0x005 | 7 |
| 1 | 0x007 | 3 | 9 | 0x070 | 7 | 17 | 0x06d | 9 | 25 | 0x02d | 7 |
| 2 | 0x004 | 3 | 10 | 0x050 | 7 | 18 | 0x09b | 9 | 26 | 0x015 | 6 |
| 3 | 0x008 | 4 | 11 | 0x0ed | 8 | 19 | 0x010 | 8 | 27 | 0x00d | 6 |
| 4 | 0x01d | 5 | 12 | 0x0a5 | 8 | 20 | 0x045 | 8 | 28 | 0x000 | 5 |
| 5 | 0x03b | 6 | 13 | 0x0c5 | 8 | 21 | 0x025 | 8 | 29 | 0x00b | 5 |
| 6 | 0x035 | 6 | 14 | 0x090 | 8 | 22 | 0x01b | 8 | 30 | 0x003 | 4 |
| 7 | 0x05b | 7 | 15 | 0x19b | 9 | 23 | 0x030 | 7 | 31 | 0x001 | 3 |

---

## 2. Type 20 — Moving Blocks Beta

`Resources/Info`: `Moving Blocks Beta`, `© Acorn Computers 1996`, `6Y6UV 6,6,6`,
`Temporal,Spatial`.

Type 20 is **type 19 with 6-bit chroma.** It reuses type 19's block grammar,
motion families/tables, **64-symbol luma Huffman table** and luma predictor
unchanged. Only the chroma changes — and it changed twice, so there are two
mutually incompatible revisions:

- **Colour is 6Y6UV:** luma is 6-bit (= type 19); U and V are signed **6-bit**
  (stored modulo 64, range −32..31; sign-extend with *half* = 32).
- The **data-block header carries chroma differently** in each revision (below).
  Everything outside the data header — opcodes, motion, luma residuals — is byte-
  for-byte type 19.

A decoder must be told which revision a stream uses; the two are not
distinguishable from the bytes alone.

### 2.1 "old" revision (v0.04, 20 Sep 1996) — direct chroma

The data header is **14 bits**: 2-bit opcode then U and V as direct 6-bit fields:

```
header = opcode | (U << 2) | (V << 8)
```

i.e. opcode in bits 0–1, U in bits 2–7, V in bits 8–13. No chroma prediction.

### 2.2 "new" revision (v0.05, 19 Nov 1996) — delta chroma

The data header is **10 bits**: 2-bit opcode then an 8-bit `uv` byte:

```
uv = (header >> 2) & 0xFF
u_code = uv & 0x0F          v_code = (uv >> 4) & 0x0F
U = (chroma_pred_U + delta_expand[u_code]) mod 64   then chroma_pred_U = U
V = (chroma_pred_V + delta_expand[v_code]) mod 64   then chroma_pred_V = V
```

Each 4-bit code indexes a signed delta table:

```
delta_expand[16] = { -32, -26, -20, -14, -8, -4, -2, -1, 0, 1, 2, 4, 8, 14, 20, 26 }
```

`chroma_pred_U` / `chroma_pred_V` are a chroma predictor pair carried across data
blocks within a frame, **0 at the start of every frame**, and advanced **only by
data blocks** (copy blocks leave them unchanged — exactly like the luma
predictor). A data block reconstructs all of its pixels with the resulting
(U, V).

### 2.3 Container aliasing

The "new" delta-chroma revision is muxed with **container video format 30** (the
AE7 header's "video format", not 20) while still being the type-20 codec; the
"old" revision uses format 20. The raw bitstream is unaffected — only the
container's format field and the decoder revision selection differ. See the
[container spec](ae7-armovie-container.md).

---

## 3. Type 7 — Moving Blocks

`Resources/Info`: `Moving Blocks`, `© Acorn Computers 1993`, `YUV 5,5,5`,
`Temporal,Spatial`.

Type 7 is the original Moving Blocks format. It shares the 4×4 grid, the
split-into-2×2 structure, the spatial reference tables and YUV555 colour, but its
opcode grammar, data coding and motion code are all its own — so it is specified
in full here.

### 3.1 Colour

YUV555: 5-bit luma (0..31), signed 5-bit U/V (same chroma model as type 17).

### 3.2 Block grammar (variable-length opcodes)

Type 7 opcodes are a prefix code, not a fixed 2-bit field, and there is **no
separate stationary opcode** — a stationary block is a *move* with the zero
vector.

A **4×4 block** reads one bit:

- `1` → **data 4×4** (§3.3).
- `0`, then a second bit:
  - second `0` (so `00`) → **move 4×4**: a format-7 motion code (§3.4) follows.
  - second `1` (so `01`) → **split** into four 2×2 children.

A **2×2 child** (inside a split) reads one bit:

- `1` → **data 2×2**.
- `0` → **move 2×2**: a format-7 motion code follows.

(A 2×2 never splits.) "Move" covers temporal, spatial and stationary copies; the
copy class and vector come entirely from the motion code.

### 3.3 Data blocks (literal luma, no prediction)

Type 7 stores luma **literally** — no predictor, no Huffman — and puts chroma
**after** the luma rather than in a front header:

- **Data 4×4:** opcode `1`, then **16** raw 5-bit luma values in raster order,
  then one 5-bit `U` and one 5-bit `V` (16×5 + 5 + 5 = 90 payload bits).
- **Data 2×2:** opcode `1`, then **4** raw 5-bit luma, then `U`:5, `V`:5.

All pixels in the block take the block's (U, V). There is **no cross-block
predictor** of any kind in type 7.

### 3.4 Motion codes (format 7)

A format-7 move code is a 2-bit family then a variable-width index:

| Family | Index | Meaning |
| ---:| --- | --- |
| 0 | — | **stationary**, vector (0,0) — 2 bits, no index |
| 1 | 3 | radius-1 ring (`ring(1, index)`) — 5 bits |
| 2 | 4 | radius-2 ring (`ring(2, index)`) — 6 bits |
| 3 | 6 | index 0–31: radius-4 ring; 32–55: radius-3 ring; 56–63: spatial table — 8 bits |

The `ring(r, ·)` enumeration and the spatial tables are the **same** as type 19
(§4.1, §4.3). Note the differences from format 19: family 3 reaches the radius-4
ring (so type 7's temporal reach is the ±4 box, versus type 19's ±8 `far`
family), and the zero vector has its own family-0 code rather than being a
separate opcode. The move's case prefix is the block opcode (`00` for a 4×4, `0`
for a 2×2) written before the family.

### 3.5 Frame model

Identical to type 19: raster scan, key vs inter frames (moves with a non-spatial
vector reference the previous reconstruction, so a key frame uses data blocks and
spatial moves only), and zero-padded byte alignment at the end.

---

## Appendix A. Provenance and corrections

Sources, all under `RiscOS/Sources/SystemRes/ARMovie/Video` in the RISC OS 2003
tree (see [methodology.md](methodology.md) for the repo and link convention):
[`Decomp7`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp7),
[`Decomp17`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp17)
and
[`Decomp20`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp20)
(each with `Resources/Info`, `bas/BatchComp,ffb`, `bas/MakeDecomp,ffb`), the
shared `ARMovie/bas/CompLib,ffb`, and the compiled `Decompress,ffd` modules.
Every fixture and full-movie cross-check decodes identically on the compiled
module and the reference decoder.

- **Versions and dates.** The compressor source strings are: type 7
  *"format 7 Version 0.05, 22 Aug 1996"* (the `Info` copyright is 1993 — type 7
  is the oldest format, but the shipped compressor is a 1996 build); type 17
  *"0.03, 27 Aug 1996"*; type 20 *"0.05, 19 Nov 1996"*. So although the type
  numbers are out of order, the modules are close contemporaries of type 19's
  0.04 (6 Sep 1996).
- **Two type-20 revisions shipped.** The source tree carries the v0.05 "new"
  (delta-chroma) module; the v0.04 "old" (direct-chroma, 20 Sep 1996) module
  shipped separately. They are bitstream-incompatible and both are implemented
  and cross-checked. See [`../type20-shipped-vs-source.md`](../type20-shipped-vs-source.md).
- **Type 17's Huffman table** is a distinct 32-symbol table (for 5-bit luma),
  verified byte-for-byte against the module — not type 19's 64-symbol table.
- **Type 7 has no stationary opcode and no luma predictor.** Both are easy to get
  wrong by analogy with type 19: a type-7 stationary block is the family-0 move
  code, and type-7 luma is literal (the predictor topology of types 17/19 does
  not apply). Established from the compiled decoder and the literal-data fixtures.
- **Opcode bit order.** As in type 19, opcodes are read LSB-first; this spec
  describes type 7's variable-length opcodes by the actual bit-by-bit decisions
  (`1` data, `00` move, `01` split) to avoid the transmission-order vs
  integer-value ambiguity (methodology §Bit order).
- **Shared tables verified once.** The ring enumeration and spatial tables are
  common to types 7/17/19; they were read out of the running decoders (the
  type-7 module let the 2×2 spatial column, mis-ordered in the docs, be probed
  directly) and are stated authoritatively in the type-19 spec.
