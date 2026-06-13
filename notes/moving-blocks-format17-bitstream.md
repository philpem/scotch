# Moving Blocks HQ Format 17 Bitstream

This note documents Replay compression type 17, `Moving Blocks HQ`, from:

- `ARMovie_2003/Video/Decomp17/Docs/Stream`
- `ARMovie_2003/Video/Decomp17/bas/BatchComp,ffb.txt`
- `ARMovie_2003/Video/Decomp17/bas/MakeDecomp,ffb.txt`

The published stream document is useful for the block and motion-code layout,
but the implementation shows an important correction: 4x4 and 2x2 "data"
blocks are not fixed 90-bit and 30-bit raw YUV records. They carry 10 fixed
U/V bits plus Huffman-coded luma prediction residuals.

## Identity

`Resources/Info` describes format 17 as:

```text
Moving Blocks HQ
Acorn Computers 1996
4;4;1280
4;4;1024
Temporal,Spatial
YUV 5,5,5
```

The compressor version string is:

```basic
Differ to format 17 Version 0.03 27th August 1996
```

The compressor and decompressor both use:

```basic
xmove%=8:ymove%=8:typea%=FALSE
```

So this note describes the type-b, 8-pixel-search variant.

## Bit Order

As with format 7, the bitstream is read least-significant-bit first. Codes in
the original document are written in read order.

For example:

```text
01
```

means the decoder reads a `0` bit first, then a `1` bit.

## Frame State

The decoder scans 4x4 blocks left to right, top to bottom. It maintains:

- the output frame currently being built;
- the previous decoded frame;
- a luma predictor state named `prev`.

At the start of every frame, `prev` is cleared:

```basic
MOV r5,#0
STR r5,prev
```

Data-coded 4x4 and 2x2 blocks update `prev`; move and stationary cases do not
appear to update it directly.

In this codec, "stationary" means a temporal copy from the previous decoded
frame at the same position.

## Top-Level 4x4 Cases

The top-level 4x4 code is:

| Code | Meaning | Extra data |
| --- | --- | --- |
| `00` | stationary 4x4 | none |
| `01` | 4x4 move/copy | move code after the top-level bits |
| `10` | data-coded 4x4 | 10 U/V bits, then 16 Huffman luma residuals |
| `11` | split into four 2x2 cases | four 2x2 case records |

This is the first major difference from format 7: format 17 has an explicit
stationary case for the common previous-frame same-position copy.

## 2x2 Sub-Block Cases

After top-level `11`, the block is decoded as four 2x2 sub-blocks. Each 2x2
case is:

| Code | Meaning | Extra data |
| --- | --- | --- |
| `00` | stationary 2x2 | none |
| `01` | data-coded 2x2 | 10 U/V bits, then 4 Huffman luma residuals |
| `1` | 2x2 move/copy | move code after the `1` bit |

The four sub-blocks are decoded in scan order. The decompressor labels them
`m0`, `m1`, `m2`, and `m3`.

A stationary 2x2 case is likewise a previous-frame same-position copy for that
sub-block.

## Motion Coding

Motion coding is shared by 4x4 and 2x2 move cases, but the case prefix differs:

- 4x4 move starts with top-level `01`;
- 2x2 move starts with sub-block code `1`.

After that prefix, the move-family coding is:

| Move-family bits | Meaning |
| --- | --- |
| `00xxx` | temporal copy with distance 1 |
| `01xxxx` | temporal copy with distance 2 |
| `10xxxxx` | spatial copy, or temporal copy with distance 3 |
| `11...` | farther temporal copy |

Because `typea%=FALSE`, the `10xxxxx` family has 5 payload bits. Payloads
`0..7` are spatial copies; payloads `8..31` are temporal distance-3 copies.

The source constructs the move tables at decompressor initialisation. The
spatial payloads are:

| Payload | 4x4 copy | 2x2 copy |
| ---: | --- | --- |
| 0 | `[S](-2,-4)` | `[S](-2,-2)` |
| 1 | `[S](-1,-4)` | `[S](-1,-2)` |
| 2 | `[S](0,-4)` | `[S](0,-2)` |
| 3 | `[S](1,-4)` | `[S](1,-2)` |
| 4 | `[S](2,-4)` | `[S](2,-2)` |
| 5 | `[S](-4,0)` | `[S](-2,-1)` |
| 6 | `[S](-4,-1)` | `[S](-2,0)` |
| 7 | `[S](-4,-2)` | `[S](-3,0)` |

The distance-1 and distance-2 temporal payloads match the tables in
`Decomp17/Docs/Stream` and the format-7 note.

The farther temporal table is generated from `xmove%=8`, `ymove%=8`, and
`typea%=FALSE`. It covers the positions outside the distance-3 square out to
an 8-pixel Chebyshev radius:

- rows `y=-8..-4`, all `x=-8..8`;
- side columns `x=-8..-4` and `x=4..8` for `y=-3..3`;
- rows `y=4..8`, all `x=-8..8`.

## Data-Coded 4x4 Blocks

The stream document says 4x4 data is `YYYYYYYYYYYYYYYYUV`, with 5-bit values.
The implementation does not store literal Y values in that order.

The actual format is:

```text
10 U V Yres0 Yres1 ... Yres15
```

where:

- `10` is the top-level data-code prefix;
- `U` is 5 bits;
- `V` is 5 bits;
- each `Yres` is one Huffman-coded 5-bit residual.

The U/V bits are stored immediately after the top-level prefix. In the decoded
pixel word they occupy bits 5..14; the decompressor extracts the 10 stream bits
and keeps them pre-shifted in that position.

The compressor chooses U and V as the average chroma of the 4x4 source block,
quantised to 5 bits.

### 4x4 Luma Prediction

Let `prev` be the frame's current luma predictor state. Let all luma arithmetic
wrap with `& 31`.

For the first row:

```text
y0 = prev + d0
y1 = y0   + d1
y2 = y1   + d2
y3 = y2   + d3
```

For each later row, the first pixel is predicted from the pixel above, and the
remaining pixels are predicted from the average of the left pixel and the pixel
above:

```text
y[row][0] = y[row-1][0] + d
y[row][1] = ((y[row][0] + y[row-1][1]) >> 1) + d
y[row][2] = ((y[row][1] + y[row-1][2]) >> 1) + d
y[row][3] = ((y[row][2] + y[row-1][3]) >> 1) + d
```

After the 4x4 block, `prev` becomes the average luma of the 16 decoded pixels:

```text
prev = sum(y0..y15) >> 4
```

This is why the encoder reconstructs the block as it writes it. If a residual
is rounded by a degraded Huffman table, the encoder must use the value the
decoder will actually reconstruct.

The portable implementation exposes this operation as
`codec_movingblockshq_decode_data4x4`. Its golden test covers predictor carry,
and an independent Unicorn test feeds the same payload to the compiled Acorn
`Decomp17/Decompress,ffd` implementation and compares native YUV555 output
byte-for-byte.

## Data-Coded 2x2 Blocks

A data-coded 2x2 block starts with sub-block code `01`, then 10 U/V bits, then
four Huffman-coded luma residuals:

```text
01 U V Yres0 Yres1 Yres2 Yres3
```

Prediction is:

```text
y0 = prev + d0
y1 = y0   + d1
y2 = y0   + d2
y3 = ((y1 + y2) >> 1) + d3
```

After the 2x2 block:

```text
prev = (y0 + y1 + y2 + y3) >> 2
```

The compressor chooses U and V as the average chroma of the 2x2 source block,
quantised to 5 bits.

The portable `codec_movingblockshq_decode_data2x2` primitive has also been
cross-checked against compiled Decomp17. The fixture splits one 4x4 parent into
four data-coded children with distinct chroma values, verifying the `m0`,
`m1`, `m2`, `m3` order as top-left, top-right, bottom-left, bottom-right.

## Portable Frame Verifier

`codec_movingblockshq_verify_frame` implements the complete type 17 frame
grammar. Types 17 and 19 share their top-level and split opcodes, motion tables,
stationary semantics, spatial-reference legality, raster scan, and zero-padding
rules, so those mechanics live in the codec-neutral `mb_frame_verify` module.
The type 17 callback retains the format-specific YUV555 Huffman predictor.

Portable tests cover data, stationary, temporal, spatial, and split modes at
both block sizes. Independent Unicorn fixtures run the same cases through the
compiled Acorn Decomp17 binary, including a spatial 2x2 copy from an earlier
quadrant in the same split parent. The harness option
`--previous-layout yuv555` packs previous frames as native
`Y[4:0],U[9:5],V[14:10]` words; using the default type 19 `6y5uv` layout
changes chroma positions and gives a false mismatch.

## Huffman Residual Coding

Residuals are 5-bit wraparound values. The encoder first masks the signed
residual:

```basic
AND r3,r3,#31
```

then indexes the Huffman table. `FNhuffcode(est,dest)` also updates `dest` to
the value the decoder will reconstruct:

```basic
ADD dest,est,r3,LSR #24
AND dest,dest,#31
```

The decompressor builds a lookup table with `maxbits%=9`. A fast table lookup
can decode up to four residual symbols at once; slow paths add remaining
symbols one at a time.

The normal Huffman table used by the decompressor is:

| Residual | Code | Bits |
| ---: | --- | ---: |
| 0 | `10` | 2 |
| 1 | `111` | 3 |
| 2 | `100` | 3 |
| 3 | `1000` | 4 |
| 4 | `11101` | 5 |
| 5 | `111011` | 6 |
| 6 | `110101` | 6 |
| 7 | `1011011` | 7 |
| 8 | `1100101` | 7 |
| 9 | `1110000` | 7 |
| 10 | `1010000` | 7 |
| 11 | `11101101` | 8 |
| 12 | `10100101` | 8 |
| 13 | `11000101` | 8 |
| 14 | `10010000` | 8 |
| 15 | `110011011` | 9 |
| 16 | `101101101` | 9 |
| 17 | `001101101` | 9 |
| 18 | `010011011` | 9 |
| 19 | `00010000` | 8 |
| 20 | `01000101` | 8 |
| 21 | `00100101` | 8 |
| 22 | `00011011` | 8 |
| 23 | `0110000` | 7 |
| 24 | `0000101` | 7 |
| 25 | `0101101` | 7 |
| 26 | `010101` | 6 |
| 27 | `001101` | 6 |
| 28 | `00000` | 5 |
| 29 | `01011` | 5 |
| 30 | `0011` | 4 |
| 31 | `001` | 3 |

The compressor source also contains two degraded 32-entry tables after the
normal table, but the current code forces:

```basic
!hufftable=hufftable%
```

and the quality-dependent table adjustment is commented out. The generated
decompressor only builds the normal 32-symbol table. Treat the degraded tables
as compressor experiments unless a later source path re-enables them.

## Size Decisions

Format 17 does not compare four 2x2 cases against a fixed 90-bit 4x4 data
cost. It calls `test16%` to compute the actual bit cost of a data-coded 4x4
using the current Huffman table:

```basic
CALLtest16%
IF !boploc-initbop% > !testcount% THEN
  ... CALLsample16%
ENDIF
```

`test16%` adds:

- 2 top-level bits;
- 10 U/V bits;
- the Huffman lengths of the 16 predicted luma residuals.

So the 2x2 split is kept only when it beats the Huffman-coded 4x4 data path.

## Bitstream Corrections To The Stream Document

The `Docs/Stream` file is right about:

- LSB-first bit order;
- top-level 4x4 case codes;
- 2x2 case codes;
- temporal/spatial copy model;
- the presence of 5-bit Y, U, and V values.

But it is misleading for data blocks:

- 4x4 data is not 16 literal 5-bit Y values followed by U and V;
- 2x2 data is not four literal 5-bit Y values followed by U and V;
- U/V precede luma residuals in the actual stream;
- luma is predictive and Huffman-coded;
- the actual data-block cost is variable.

For a C encoder, implementing the documented top-level syntax is not enough:
the `prev` predictor, residual Huffman table, and exact reconstruction feedback
are required for valid format-17 output.
