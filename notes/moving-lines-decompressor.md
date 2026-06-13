# Moving Lines Decompressor

This note documents the decompressor side of Acorn Replay type 1,
`Moving Lines`, from:

- `ARMovie_2003/Video/MovingLine/bas/MakeDecomp,ffb.txt`
- `ARMovie_RO371/MakeDecomp,ffb.txt`

It complements:

- `notes/moving-lines-bitstream-first-pass.md`
- `notes/moving-lines-compressor-process.md`

## Generated Variants

The 2003 source builds three decompressor binaries from the same assembler
template:

```basic
PROCass(4) -> Video.MovingLine.Decompress
PROCass(2) -> Video.MovingLine.DecompresH
PROCass(1) -> Video.MovingLine.DecompresB
```

The `size%` argument is the output pixel size in bytes:

| `size%` | Output style |
| ---: | --- |
| 4 | word-per-pixel output |
| 2 | halfword-per-pixel output |
| 1 | byte-per-pixel output |

The bitstream syntax is shared. Most of the source bulk is specialised copy and
store code for those output sizes and their alignment cases.

RO3.71 only has one `MakeDecomp,ffb.txt` at the top of the `ARMovie_RO371`
tree, and generated runtime files live under `Resources/MovingLine`.

## Interface

The generated decompressor begins with:

1. a patch-table offset word;
2. an init branch;
3. a decompress entry branch.

The source comments describe the important frame-decode registers:

```text
r0 - source byte pointer, halfword aligned
r1 - output pointer
r2 - previous output pointer, later converted to an inter-buffer offset
r3 - pixel dither/lookup table
r4 - return address
r5 - temporal/spatial offset table
r6 - 0x7fff mask
```

At the decompressor entry:

```basic
SUB r2,r2,r1       REM convert previous pointer to offset from current
ADR r5,offtab
MOV r6,#&ff
ORR r6,r6,#&7f00  REM r6 = &7fff
B decl
```

For temporal copies, the decoder starts from the current output pointer plus an
offset-table entry, then adds `r2` to reach the previous frame. For spatial
copies, it uses the current output frame directly.

## Offset Table Initialisation

`init` builds `offtab` from the movie width passed in `r0`.

Temporal entries are generated first:

```basic
FOR y = -move TO move
  FOR x = -move TO move
    IF (x,y) <> (0,0)
      offset = y * width + x
      offset *= size
      store offset
```

The centre temporal entry is skipped. This matches the compressor: ordinary
temporal copy codes exclude same-position previous-frame copies, because those
are encoded with the `0x1e` same-position run family.

With `move%=8`, temporal offsets cover a 17x17 square minus the centre point,
giving 288 temporal table entries, addressed as code range `0x000..0x11f`.

Spatial entries are generated next:

```basic
FOR y = -smove TO -1
  FOR x = -smove TO smove
    offset = y * width + x
    offset *= size
    store offset
```

With `smove%=9`, spatial offsets cover 9 previous rows by 19 horizontal
positions, giving 171 spatial entries, addressed as code range `0x120..0x1ca`.

The compressor and comments describe the spatial family as running through
`0x1cb`. The generated table reserve covers entries up to `0x1cb`, but the
source loops produce 171 spatial offsets, so `0x1cb` looks like a reserved or
unused upper slot at this pass.

The `PROCg` helper at the bottom of `MakeDecomp` uses `sx%=160`; that is not
the runtime rule. The real init code uses the width supplied in `r0`.

## Patch Table And Pixel Lookup

`FNplook(rn)` emits either a no-op `MOV rn,rn` or records a patch-table entry
for Player to replace with colour lookup code:

```basic
!tablestart%=&0000<<28 OR rn<<24 OR rn<<20 OR 3<<16 OR (P%-code%)
```

The source comments say opcode 0 patches in colour lookup. Moving Lines source
pixels are 15-bit RGB or YUV values; patched playback can map them through a
display table.

For C tooling, this means the bitstream carries 15-bit source pixels. Display
conversion is a playback concern unless a C verifier wants to emulate a
particular patched output mode.

## Main Decode Dispatch

The main loop reads one halfword:

```basic
LDR r8,[r0],#2
ANDS r7,r6,r8,LSR #1
BCC decp
CMP r7,#&1cc<<6
BCS decs
```

Interpreted as C-like logic:

```c
word = read16();
payload = (word >> 1) & 0x7fff;

if ((word & 1) == 0) {
    decode_literal_pixel(payload);
} else if (payload < (0x1cc << 6)) {
    decode_temporal_or_spatial_copy(payload);
} else {
    decode_special(payload);
}
```

`decp` applies optional pixel lookup and stores one output pixel.

## Temporal And Spatial Copy Decode

The copy path splits the payload:

```basic
AND r10,r7,#63
ADD r10,r10,#2      REM length
MOV r7,r7,LSR #6    REM direction/code
LDR r9,[r5,r7,LSL #2]
ADD r7,r9,r1
ADDCC r7,r7,r2      REM previous-frame source if code < &120
```

The carry flag comes from:

```basic
CMP r7,#&120<<6
```

So:

- code `< 0x120`: copy from previous frame;
- code `>= 0x120`: copy from current output frame.

The decoded length is `2..65` pixels.

The following code copies from `r7` to `r1`, using hand-optimised alignment
paths for `size%` 1, 2, and 4.

## Same-Position Previous-Frame Run

Special payloads at or above `0x1e << 10` use a 10-bit length field:

```basic
decs:
  CMP r7,#&1e<<10
  BCC decspecial
  AND r10,r7,r6,LSR #5  REM r10 = payload & &3ff
  CMP r7,#&1f<<10
  BCS decn
  ADD r10,r10,#1
  ADD r7,r1,r2
  copy r10 pixels
```

For family `0x1e`, the source pointer is `r1 + r2`, which is the same output
position in the previous frame. The decoded length is `1..1024` pixels.

This is the command the compressor comments call "skip"; it is really a
same-position previous-frame copy.

## Long Literal Run

Family `0x1f` branches to `decn`. The length field is:

```text
length = (payload & 0x3ff) + 1
```

`decn` turns the byte pointer into a bit pointer:

```basic
MOV r0,r0,LSL #3
```

Then it extracts packed 15-bit pixels. The fast loop reads four pixels at a
time from three words, separates them into registers, applies pixel lookup, and
stores them in the selected output size.

After the last pixel, the decoder realigns to the next halfword boundary:

```basic
ADD r0,r0,#15
BIC r0,r0,#15
MOV r0,r0,LSR #3
```

Then it resumes normal halfword command decoding.

## Repeat Pixel And End Of Frame

Payloads below `0x1e << 10` but at or above `0x1cc << 6` enter
`decspecial`:

```basic
decspecial:
  CMP r7,#&1cc<<6
  MOVEQ pc,r4
  LDR r8,[r0],#2
  AND r8,r8,r6
  FNplook(8)
  AND r7,r7,#63
  ADD r7,r7,#2
  repeat pixel r7 times
```

If the payload equals `0x1cc << 6`, the frame is complete and the decompressor
returns via `r4`.

Otherwise, it reads one following halfword as the repeated 15-bit pixel and
outputs it `length_field + 2` times.

The compressor emits repeat-pixel runs with family `0x1cc` only. The
decompressor would also repeat for other payloads in this special range, because
the historical guard against `0x1cd` and above is commented out:

```basic
REM      CMP r7,#&1cd<<6
REM      BCS decspecial2
```

Treat `0x1cc` repeats as the supported encoder output; other special-family
repeat aliases are decoder-tolerated but not known to be intentionally used.

## Output Size Differences

All variants decode the same commands. The differences are only in how decoded
pixels and copy runs are written:

- `size%=4`: output pixels are stored as words; this is the main
  `Decompress` variant.
- `size%=2`: output pixels are stored as two bytes, with alignment paths for
  halfword output.
- `size%=1`: output pixels are stored as bytes, with alignment paths for packed
  byte output.

The source comments also state:

```basic
The MovingLines decompressor saves pixels as Words!!!
```

That comment describes the main Moving Lines usage. The 2003 source can still
generate byte and halfword variants for other playback/display paths.

## Complete Code Family Decode

| Payload condition | Decode action |
| --- | --- |
| input word bit 0 clear | one literal 15-bit pixel |
| payload `< 0x120 << 6` | temporal copy, length `(payload & 63) + 2` |
| payload `< 0x1cc << 6` | spatial copy, length `(payload & 63) + 2` |
| payload `== 0x1cc << 6` | end of frame |
| payload `< 0x1e << 10` | repeated pixel, length `(payload & 63) + 2`, followed by one pixel halfword |
| payload `< 0x1f << 10` | same-position previous-frame copy, length `(payload & 0x3ff) + 1` |
| payload `>= 0x1f << 10` | packed literal run, length `(payload & 0x3ff) + 1`, followed by packed 15-bit pixels |

The ordering matters: the end-of-frame check occurs before repeat-pixel output.

## C Verifier Implications

A C verifier for Moving Lines can be much simpler than the ARM decompressor:

- build the same offset table using frame width and output pixel size;
- decode into a normal word-per-pixel reconstruction buffer;
- ignore byte/halfword display variants initially;
- implement pixel lookup as identity;
- implement temporal copies by reading from the previous reconstructed frame;
- implement spatial copies by reading from the current reconstructed frame.

That is enough to validate compressor output. Display-specific byte and
halfword paths can be treated as player/runtime behaviour rather than encoder
requirements.

