# Super Moving Blocks Format 19 Bitstream

This note documents Replay compression type 19, `Super Moving Blocks`, from:

- `ARMovie_2003/Video/Decomp19/Resources/Info`
- `ARMovie_2003/Video/Decomp19/bas/BatchComp,ffb.txt`
- `ARMovie_2003/Video/Decomp19/bas/MakeDecomp,ffb.txt`

There is no separate `Docs/Stream` file for `Decomp19` in this tree. The
format is therefore inferred from the compressor and decompressor source.

## Identity

`Resources/Info` describes format 19 as:

```text
Super Moving Blocks
Acorn Computers 1996
,C
4;4;1280
4;4;1024
Temporal,Spatial
6Y5UV 6,5,5
```

The compressor version string is:

```basic
Differ to format 19 Version 0.04 6th September 1996
```

The compressor asks CompLib for:

```basic
convertto$="6Y5UV"
```

The motion-search setup is the same as format 17:

```basic
xmove%=8:ymove%=8:typea%=FALSE
```

## Relationship To Format 17

Format 19 keeps the Moving Blocks HQ block syntax:

| 4x4 code | Meaning |
| --- | --- |
| `00` | stationary 4x4 |
| `01` | 4x4 move/copy |
| `10` | data-coded 4x4 |
| `11` | split into four 2x2 cases |

and the same 2x2 syntax:

| 2x2 code | Meaning |
| --- | --- |
| `00` | stationary 2x2 |
| `01` | data-coded 2x2 |
| `1` | 2x2 move/copy |

Motion coding and spatial payloads follow the same type-b `xmove%=8`,
`ymove%=8` scheme documented in
`notes/moving-blocks-format17-bitstream.md`.

The differences are in source representation and data-block coding:

- Y is 6-bit, not 5-bit;
- U and V are still 5-bit;
- luma residuals are 6-bit wraparound values;
- the Huffman table has 64 symbols and `maxbits%=11`.

## Data-Coded 4x4 Blocks

A data-coded 4x4 block is:

```text
10 U V Yres0 Yres1 ... Yres15
```

where:

- `10` is the top-level data prefix;
- `U` is 5 literal bits;
- `V` is 5 literal bits;
- each `Yres` is one Huffman-coded 6-bit residual.

The compressor writes the prefix and chroma together as 12 bits:

```basic
MOV r2,r4,LSL #2
ORR r2,r2,#%01
MOV r3,#12       REM 2 bits start plus 10 for UV
BL  storebits%
```

The reconstructed pixel packs Y in bits 0..5 and U/V above it; the compressor
reconstructs with:

```basic
ORR r6,r6,r4,LSL #6
```

The 4x4 luma predictor is the same as format 17, except all luma arithmetic is
masked to six bits:

```text
y0 = prev + d0
y1 = y0   + d1
y2 = y1   + d2
y3 = y2   + d3
```

Then for later rows:

```text
y[row][0] = y[row-1][0] + d
y[row][1] = ((y[row][0] + y[row-1][1]) >> 1) + d
y[row][2] = ((y[row][1] + y[row-1][2]) >> 1) + d
y[row][3] = ((y[row][2] + y[row-1][3]) >> 1) + d
```

After the block:

```text
prev = sum(y0..y15) >> 4
```

## Data-Coded 2x2 Blocks

A data-coded 2x2 block is:

```text
01 U V Yres0 Yres1 Yres2 Yres3
```

where U and V are literal 5-bit values and each residual is Huffman-coded from
the 64-symbol luma table.

Prediction is:

```text
y0 = prev + d0
y1 = y0   + d1
y2 = y0   + d2
y3 = ((y1 + y2) >> 1) + d3
```

After the 2x2:

```text
prev = (y0 + y1 + y2 + y3) >> 2
```

## Huffman Residual Coding

The decompressor uses:

```basic
maxbits%=11
```

and builds a 64-symbol table. The table is also used by the compressor's
`FNhuffcode` and `FNhufflen`; both mask residuals with `#63`.

The table is:

| Residual | Code | Bits |
| ---: | --- | ---: |
| 0 | `10` | 2 |
| 1 | `111` | 3 |
| 2 | `1101` | 4 |
| 3 | `11001` | 5 |
| 4 | `11100` | 5 |
| 5 | `11000` | 5 |
| 6 | `110001` | 6 |
| 7 | `110100` | 6 |
| 8 | `110000` | 6 |
| 9 | `1100001` | 7 |
| 10 | `1101100` | 7 |
| 11 | `1010000` | 7 |
| 12 | `11000001` | 8 |
| 13 | `11001100` | 8 |
| 14 | `11101000` | 8 |
| 15 | `110100001` | 9 |
| 16 | `110001100` | 9 |
| 17 | `111010100` | 9 |
| 18 | `110010100` | 9 |
| 19 | `110010000` | 9 |
| 20 | `1010100001` | 10 |
| 21 | `1101000001` | 10 |
| 22 | `1101001100` | 10 |
| 23 | `1010010100` | 10 |
| 24 | `1100010100` | 10 |
| 25 | `10101000001` | 11 |
| 26 | `00101000001` | 11 |
| 27 | `11010001100` | 11 |
| 28 | `10101001100` | 11 |
| 29 | `00101001100` | 11 |
| 30 | `10010010100` | 11 |
| 31 | `11000010100` | 11 |
| 32 | `11010010000` | 11 |
| 33 | `01010010000` | 11 |
| 34 | `11100010000` | 11 |
| 35 | `01100010000` | 11 |
| 36 | `10000010100` | 11 |
| 37 | `00000010100` | 11 |
| 38 | `01000010100` | 11 |
| 39 | `00010010100` | 11 |
| 40 | `01010001100` | 11 |
| 41 | `0010010000` | 10 |
| 42 | `0100010000` | 10 |
| 43 | `0100010100` | 10 |
| 44 | `0010001100` | 10 |
| 45 | `0010100001` | 10 |
| 46 | `000010000` | 9 |
| 47 | `011010100` | 9 |
| 48 | `001001100` | 9 |
| 49 | `001000001` | 9 |
| 50 | `01101000` | 8 |
| 51 | `01010100` | 8 |
| 52 | `00001100` | 8 |
| 53 | `00100001` | 8 |
| 54 | `0101000` | 7 |
| 55 | `0101100` | 7 |
| 56 | `0000001` | 7 |
| 57 | `001000` | 6 |
| 58 | `010001` | 6 |
| 59 | `00000` | 5 |
| 60 | `00100` | 5 |
| 61 | `01001` | 5 |
| 62 | `0101` | 4 |
| 63 | `011` | 3 |

## Size Decisions

Like format 17, format 19 uses `test16%` to compare a four-2x2 split against
the actual Huffman-coded 4x4 data cost. For format 19, that cost is:

- 2 top-level bits;
- 10 literal U/V bits;
- the Huffman lengths of 16 six-bit luma residuals.

So the data cost is variable, and an encoder must calculate it from the same
prediction and Huffman table the decoder uses.

## Implementation Notes

Format 19 is a cleaner extension of format 17 than format 20:

- it has no `D4tab`;
- it has no `prevu`/`prevv` state;
- chroma is literal inside data-coded blocks;
- only the luma precision and Huffman table change materially.

That makes it the best first target for a portable C Moving Blocks compressor:
it exercises the later block machinery without format 20's chroma predictor
state.
