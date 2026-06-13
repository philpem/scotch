# Moving Blocks Format 7 Bitstream

This note transcribes the format 7 Moving Blocks bitstream from
`ARMovie_2003/Video/Decomp7/Docs/Stream`, with cross-checks against
`ARMovie_2003/Video/Decomp7/bas/BatchComp,ffb.txt`.

This is for Replay compression type 7, `Moving Blocks`, not Moving Blocks HQ
(`Decomp17`), whose top-level codes differ.

## Bit Order

The stream is a bitstream with the least significant bit arriving first.

The original document writes binary codes in read order. For example, a code
shown as:

```text
01xxx
```

means the decoder first reads `0`, then `1`, then three payload bits.

## Frame Scan

Frames are decoded as 4x4 pixel blocks, scanned left to right and top to
bottom.

Spatial copies refer to already-decoded parts of the current frame. Therefore
spatial source areas must be non-overlapping and above and/or left of the
destination.

Temporal copies refer to the previous decoded frame.

## Component Packing

All format 7 raw pixel data uses 5-bit YUV components.

### Raw 4x4 Block

Raw 4x4 data contains:

```text
Y0 Y1 Y2 Y3
Y4 Y5 Y6 Y7
Y8 Y9 Y10 Y11
Y12 Y13 Y14 Y15
U V
```

Each value is 5 bits, so the raw block cost is:

```text
16 * 5 + 2 * 5 = 90 bits
```

The original document labels the fourth row as `12 14 15 16`; that appears to
be a typo for `13 14 15 16` when using one-based labels.

### Raw 2x2 Block

Raw 2x2 data contains:

```text
Y0 Y1
Y2 Y3
U V
```

Each value is 5 bits, so the raw sub-block cost is:

```text
4 * 5 + 2 * 5 = 30 bits
```

## Top-Level 4x4 Codes

For each 4x4 destination block:

| Code | Meaning | Extra data |
| --- | --- | --- |
| `1` | raw 4x4 data | 90 raw bits |
| `00` | 4x4 move/copy case | move code |
| `01` | split into four 2x2 cases | four 2x2 case records |

There is no explicit 4x4 stationary top-level code in format 7. A stationary
4x4 block is represented as a move case with temporal offset `(0,0)`.

## 2x2 Sub-Block Codes

Inside a split 4x4 block, each 2x2 sub-block is encoded as:

| Code | Meaning | Extra data |
| --- | --- | --- |
| `1` | raw 2x2 data | 30 raw bits |
| `0` | 2x2 move/copy case | move code |

Again, stationary 2x2 blocks are represented by move codes rather than a
separate top-level stationary code.

## Move Code Families

Move codes are shared by 4x4 and 2x2 cases:

| Code | Meaning |
| --- | --- |
| `00` | temporal copy from same position, `[T](0,0)` |
| `01xxx` | temporal copy where `max(abs(dx), abs(dy)) = 1` |
| `10xxxx` | temporal copy where `max(abs(dx), abs(dy)) = 2` |
| `11xxxxxx` | temporal copy at distance 3/4, or spatial copy |

`[T](x,y)` means copy from the previous frame.  
`[S](x,y)` means copy from the current frame.

Negative `x` means source is left of the destination. Negative `y` means source
is above the destination.

## `01xxx` Table

These are temporal copies at distance 1:

| Payload | Copy |
| ---: | --- |
| 0 | `[T](-1,-1)` |
| 1 | `[T](0,-1)` |
| 2 | `[T](1,-1)` |
| 3 | `[T](-1,0)` |
| 4 | `[T](1,0)` |
| 5 | `[T](-1,1)` |
| 6 | `[T](0,1)` |
| 7 | `[T](1,1)` |

## `10xxxx` Table

These are temporal copies at distance 2:

| Payload | Copy |
| ---: | --- |
| 0 | `[T](-2,-2)` |
| 1 | `[T](-1,-2)` |
| 2 | `[T](0,-2)` |
| 3 | `[T](1,-2)` |
| 4 | `[T](2,-2)` |
| 5 | `[T](-2,-1)` |
| 6 | `[T](2,-1)` |
| 7 | `[T](-2,0)` |
| 8 | `[T](2,0)` |
| 9 | `[T](-2,1)` |
| 10 | `[T](2,1)` |
| 11 | `[T](-2,2)` |
| 12 | `[T](-1,2)` |
| 13 | `[T](0,2)` |
| 14 | `[T](1,2)` |
| 15 | `[T](2,2)` |

## `11xxxxxx` Table

Payloads 0-55 are temporal copies:

| Payload | Copy |
| ---: | --- |
| 0 | `[T](-4,-4)` |
| 1 | `[T](-3,-4)` |
| 2 | `[T](-2,-4)` |
| 3 | `[T](-1,-4)` |
| 4 | `[T](0,-4)` |
| 5 | `[T](1,-4)` |
| 6 | `[T](2,-4)` |
| 7 | `[T](3,-4)` |
| 8 | `[T](4,-4)` |
| 9 | `[T](-4,-3)` |
| 10 | `[T](4,-3)` |
| 11 | `[T](-4,-2)` |
| 12 | `[T](4,-2)` |
| 13 | `[T](-4,-1)` |
| 14 | `[T](4,-1)` |
| 15 | `[T](-4,0)` |
| 16 | `[T](4,0)` |
| 17 | `[T](-4,1)` |
| 18 | `[T](4,1)` |
| 19 | `[T](-4,2)` |
| 20 | `[T](4,2)` |
| 21 | `[T](-4,3)` |
| 22 | `[T](4,3)` |
| 23 | `[T](-4,4)` |
| 24 | `[T](-3,4)` |
| 25 | `[T](-2,4)` |
| 26 | `[T](-1,4)` |
| 27 | `[T](0,4)` |
| 28 | `[T](1,4)` |
| 29 | `[T](2,4)` |
| 30 | `[T](3,4)` |
| 31 | `[T](4,4)` |
| 32 | `[T](-3,-3)` |
| 33 | `[T](-2,-3)` |
| 34 | `[T](-1,-3)` |
| 35 | `[T](0,-3)` |
| 36 | `[T](1,-3)` |
| 37 | `[T](2,-3)` |
| 38 | `[T](3,-3)` |
| 39 | `[T](-3,-2)` |
| 40 | `[T](3,-2)` |
| 41 | `[T](-3,-1)` |
| 42 | `[T](3,-1)` |
| 43 | `[T](-3,0)` |
| 44 | `[T](3,0)` |
| 45 | `[T](-3,1)` |
| 46 | `[T](3,1)` |
| 47 | `[T](-3,2)` |
| 48 | `[T](3,2)` |
| 49 | `[T](-3,3)` |
| 50 | `[T](-2,3)` |
| 51 | `[T](-1,3)` |
| 52 | `[T](0,3)` |
| 53 | `[T](1,3)` |
| 54 | `[T](2,3)` |
| 55 | `[T](3,3)` |

Payloads 56-63 are spatial copies. The offset meaning depends on whether the
move applies to a 4x4 block or a 2x2 sub-block:

| Payload | 4x4 copy | 2x2 copy |
| ---: | --- | --- |
| 56 | `[S](-2,-4)` | `[S](-2,-2)` |
| 57 | `[S](-1,-4)` | `[S](-1,-2)` |
| 58 | `[S](0,-4)` | `[S](-2,-1)` |
| 59 | `[S](1,-4)` | `[S](0,-2)` |
| 60 | `[S](2,-4)` | `[S](1,-2)` |
| 61 | `[S](-4,0)` | `[S](2,-2)` |
| 62 | `[S](-4,-1)` | `[S](-2,0)` |
| 63 | `[S](-4,-2)` | `[S](-3,0)` |

The 2x2 spatial ordering in the prose list near the top of the original stream
document differs from this final table. The encoder source matches the final
detailed table above.

## Encoder Code Mapping

The format-7 encoder builds move-code tables in `BatchComp,ffb.txt`:

- `n16%`, `c16%`: bit lengths and codes for 4x4 temporal moves.
- `n4%`, `c4%`: bit lengths and codes for 2x2 temporal moves.
- `centre%`: temporal `(0,0)`, encoded as the stationary move.

Spatial copies are emitted directly:

- 4x4 spatial codes use 10 total bits in the 4x4 move path.
- 2x2 spatial codes use 9 total bits in the 2x2 move path, because the 2x2
  move/data decision has already supplied one leading move bit.

The decoder and encoder both treat payloads 56-63 as spatial; all lower
payloads in the `11xxxxxx` family are temporal.

## Differences From Moving Blocks HQ

`Decomp17/Docs/Stream` starts from the same conceptual model but uses different
top-level codes:

```text
00  4x4 stationary block
01  4x4 move case
10  4x4 data
11  4x4 split into 2x2 cases
```

and different 2x2 case codes:

```text
00  2x2 stationary block
01  2x2 data
1   2x2 move case
```

So this note should not be used as a Decomp17 bitstream reference.
