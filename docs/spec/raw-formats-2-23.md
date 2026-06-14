# Uncompressed frame formats — types 2 and 23

Authoritative specification of the two **uncompressed** ARMovie video formats.
Conventions (LSB-first bit order, signed chroma) are in
[methodology.md](methodology.md). Neither format has a block grammar, motion or
inter-frame prediction: every frame is a fixed-size raw buffer, so the frame
count is `total payload bytes / frame size` and a frame is extracted by offset.

## 1. Type 2 — 16-bit colour, uncompressed

`Decomp2/Resources/Info`: `16 bit colour uncompressed`, `© Acorn Computers 1993`,
pixel depth `16`, colour `YUV 5,5,5; RGB 5,5,5; 6Y5UV 6,5,5`.

Each frame is `width × height` **16-bit little-endian** words, one per pixel, in
raster order. Frame size is `width × height × 2` bytes, identical for every
frame.

Type 2 is a *carrier* for any of three 16-bit colour models; which one applies is
the movie's declared colour, not something in the pixel data. The bit layout
within each 16-bit word:

| Colour model | Layout (bit ranges within the word) |
| --- | --- |
| 6Y5UV (6,5,5) | Y = bits 0–5 (6-bit), U = bits 6–10, V = bits 11–15 (signed 5-bit) |
| YUV 5,5,5 | Y = bits 0–4, U = bits 5–9, V = bits 10–14 (top bit unused/by convention) |
| RGB 5,5,5 | R/G/B 5-bit fields (a true RGB sample, not YUV) |

For 6Y5UV the layout is exactly the packed-6Y5UV word used by the Moving Blocks
working format (methodology §Colour); U and V are signed (modulo 32). A decoder
must apply the model the header declares — the words themselves are
indistinguishable.

## 2. Type 23 — 6Y6Y5U5V, uncompressed (4:2:2)

`Decomp23/Resources/Info`: `6Y6Y5U5V uncompressed`, `© Acorn Computers 1993`,
pixel depth `11`, colour `YUV 6,5,5`.

Type 23 is uncompressed **4:2:2**: horizontally adjacent pixels are coded in
**pairs** that share one chroma sample (2:1 horizontal subsampling; chroma is
*not* subsampled vertically). Each pair is **22 bits** = 11 bits/pixel. Width
must be even.

Within a pair, read least-significant bit first, the 22 bits are:

| Field | Bits | Width | Meaning |
| --- | --- | ---:| --- |
| Y0 | 0–5 | 6 | luma of the left pixel |
| Y1 | 6–11 | 6 | luma of the right pixel |
| U | 12–16 | 5 | signed chroma shared by both pixels |
| V | 17–21 | 5 | signed chroma shared by both pixels |

(`6Y6Y5U5V` = two 6-bit lumas then a 5-bit U and 5-bit V.) U and V are the signed
5-bit chroma model (modulo 32). The two pixels of a pair get the same (U, V); an
encoder averages each pair's source U and V (floor toward −∞, the ARM-ASR
average).

The 22-bit pairs are concatenated **LSB-first** across the frame with no
per-pair padding; the **frame as a whole** is padded to a byte boundary, so a
frame is `ceil(width × height × 11 / 8)` bytes. Pairs are taken in raster order
(left-to-right, top-to-bottom).

## Appendix A. Provenance and corrections

Sources (RISC OS 2003 tree; see [methodology.md](methodology.md) for the link
convention):
[`Decomp2`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp2)
and
[`Decomp23`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp23)
(`Resources/Info` plus the compiled modules). Both are dated © 1993.

- **Type 2 is colour-model-agnostic.** "16 bit colour uncompressed" stores raw
  16-bit words whose meaning (6Y5UV, YUV555 or RGB555) comes from the movie's
  declared colour, not the data. The reference tooling unpacks type 2 through the
  **6Y5UV** field layout (Y:6 / U:5 / V:5) because that is the interpretation the
  type-19 path needs for cross-checking; that is a deliberate reinterpretation of
  the halfword, not a colour-space conversion, and a general type-2 reader must
  honour the declared model instead of assuming 6Y5UV.
- **Type 23 chroma is horizontal-only 4:2:2.** Each *pair* shares U/V; rows are
  full chroma resolution. The 22-bit pair packing and field order (Y0, Y1, U, V,
  LSB-first) were read from the module and verified against the reference
  unpack/pack; the per-frame (not per-pair) byte padding is what makes the frame
  size `ceil(W·H·11/8)`.
- **No temporal dependency.** Unlike the Moving Blocks family, every type-2 and
  type-23 frame is self-contained, so any frame is independently decodable and
  seeking is exact from the catalogue.
