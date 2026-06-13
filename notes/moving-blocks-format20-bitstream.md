# Moving Blocks Beta Format 20 Bitstream

This note documents Replay compression type 20, `Moving Blocks Beta`, from:

- `ARMovie_2003/Video/Decomp20/Resources/Info`
- `ARMovie_2003/Video/Decomp20/bas/BatchComp,ffb.txt`
- `ARMovie_2003/Video/Decomp20/bas/MakeDecomp,ffb.txt`

There is no separate `Docs/Stream` file for `Decomp20` in this tree. The
format is inferred from the compressor and decompressor source.

## Identity

`Resources/Info` describes format 20 as:

```text
Moving Blocks Beta
Acorn Computers 1996
,C
4;4;1280
4;4;1024
Temporal,Spatial
6Y6UV 6,6,6
```

The compressor version string is:

```basic
Differ to format 20 Version 0.05 19th November 1996
```

The compressor asks CompLib for:

```basic
convertto$="6Y6UV"
```

## Relationship To Format 19

Format 20 keeps the same Moving Blocks HQ block and motion syntax as formats
17 and 19:

| 4x4 code | Meaning |
| --- | --- |
| `00` | stationary 4x4 |
| `01` | 4x4 move/copy |
| `10` | data-coded 4x4 |
| `11` | split into four 2x2 cases |

| 2x2 code | Meaning |
| --- | --- |
| `00` | stationary 2x2 |
| `01` | data-coded 2x2 |
| `1` | 2x2 move/copy |

It also keeps the format-19 luma model:

- 6-bit Y;
- 64-symbol luma residual Huffman table;
- `maxbits%=11`;
- the same 4x4 and 2x2 luma prediction rules.

The big difference is chroma. Format 20 is `6Y6UV`, but data-coded blocks do
not store literal 6-bit U plus literal 6-bit V. They store two 4-bit chroma
delta codes, one for U and one for V.

## Chroma State

The decompressor has persistent per-frame chroma predictor state:

```basic
.prevu DCB 0
.prevv DCB 0
```

At frame start it clears `prev` and `prevu`:

```basic
MOV r5,#0
STR r5,prev
STR r5,prevu
```

Because `prevu` and `prevv` are adjacent bytes, storing the word zero at
`prevu` clears both states.

The compressor mirrors this:

```basic
!prev=0:?prevu=0:?prevv=0
```

and explicitly saves/restores `prevu` when trying 2x2 subdivision versus raw
4x4:

```basic
initprevuv%=!prevu
...
!prevu=initprevuv%
```

## Data-Coded 4x4 Blocks

A data-coded 4x4 block is:

```text
10 Udelta Vdelta Yres0 Yres1 ... Yres15
```

where:

- `10` is the top-level data prefix;
- `Udelta` is 4 bits;
- `Vdelta` is 4 bits;
- each `Yres` is one Huffman-coded 6-bit residual.

The compressor writes the prefix and chroma delta byte together as 10 bits:

```basic
BL chromapack
MOV r2,r2,LSL #2
ORR r2,r2,#%01
MOV r3,#10
BL  storebits%
```

The decompressor advances by the same 10 bits:

```basic
ADD r0,r0,#8+2
BL unpackuv
```

## Data-Coded 2x2 Blocks

A data-coded 2x2 block is:

```text
01 Udelta Vdelta Yres0 Yres1 Yres2 Yres3
```

The compressor writes:

```basic
BL chromapack
MOV r2,r2,lsl #2
ORR r2,r2,#%10
MOV r3,#10
BL  storebits%
```

The decompressor has already accounted for the 2x2 case bits, so each clear
2x2 path advances by 8 chroma bits before decoding luma:

```basic
ADD r0,r0,#8
BL unpackuv
```

## Chroma Delta Expansion

The transmitted 4-bit chroma code is expanded through this signed table:

```text
0  -> -32
1  -> -26
2  -> -20
3  -> -14
4  ->  -8
5  ->  -4
6  ->  -2
7  ->  -1
8  ->   0
9  ->   1
10 ->   2
11 ->   4
12 ->   8
13 ->  14
14 ->  20
15 ->  26
```

The decompressor applies the deltas modulo 64:

```text
u = (prevu + delta[Udelta]) & 63
v = (prevv + delta[Vdelta]) & 63
prevu = u
prevv = v
```

Then it packs reconstructed chroma as:

```basic
ORR r12,r5,r6,LSL #6
MOV r12,r12,LSL #6
```

so reconstructed pixels carry 6-bit Y plus corrected 6-bit U/V.

## Compressor Chroma Packing

The compressor computes a target average 6-bit U/V for the block, then calls
`chromapack`. `chromapack` uses the external table:

```basic
OSCLI"Load <ARMovie$Dir>.Decomp20.D4tab "+STR$~deltatable
```

The table maps `(previous_chroma, target_chroma)` to a 4-bit delta code. It is
stored as packed nibbles:

```basic
ORR r7,r6,r5,LSL #6
TST r7,#1
LDRB r7,[r1,r7,LSR #1]
MOVNE r7,r7,LSR #4
AND r2,r7,#15
```

After choosing a code, the compressor expands it through the same `deltaexpand`
table and stores the corrected future predictor:

```basic
LDRB r7,[r10,r2]
ADD r5,r6,r7
AND r5,r5,#63
STRB r5,prevu
```

It repeats the same process for V, placing the V code in the high nibble of the
transmitted byte. This means the encoder does not necessarily reconstruct the
requested average chroma. It reconstructs the nearest value reachable by the
4-bit delta table, then uses that corrected value for future state.

## Luma Coding

Luma is the same as format 19:

- 6-bit values;
- residuals masked with `#63`;
- the same 64-symbol Huffman table;
- same 4x4 and 2x2 prediction rules;
- `prev = average luma` after each data-coded block.

The Huffman table is identical to the table in
`notes/moving-blocks-format19-bitstream.md`.

## Size Decisions

Format 20 uses `test16%`, but the raw 4x4 cost differs from format 19:

- 2 top-level bits;
- 8 chroma delta bits;
- the Huffman lengths of 16 six-bit luma residuals.

The source also checks whether a single average chroma value has too much
variance against quadrant chroma averages. That extra logic is part of deciding
whether four 2x2 data blocks are worth keeping, because 2x2 blocks can carry
separate chroma deltas.

## Implementation Notes

Format 20 is more complex than format 19:

- the chroma predictor state is part of the bitstream state;
- `D4tab` is required to reproduce the compressor's choice of 4-bit chroma
  deltas;
- encoder trial paths must save and restore chroma state, not just luma state;
- generated output must use corrected reconstructed chroma, not merely the
  source block's average chroma.

This makes format 20 a poor first target for portable C compression, despite
its close relationship to format 19.
