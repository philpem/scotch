# Eidos "Escape" codecs (Replay video formats 100–199)

The Escape family is the **Eidos** range of Replay/ARMovie codecs (numbers
100–199), distinct from the Acorn-native types. Escape began as a video codec on
Acorn RISC OS (Eidos Technologies started as an Acorn video-software house) and
**survived the company's merger into Eidos Interactive**, where the same codec
family was reused for full-motion video in PC games — which is why FFmpeg and
Nihav carry decoders for the later revisions.

Status:

| Type | Name | Internal format | Our support |
| --- | --- | --- | --- |
| 100 | Escape 100 | 160×128 YUV555, 2×2-block VQ; 256-entry chroma codebook; delta/skip-coded | **implemented** — native decoder (`replay_esc100`) |
| 102 | Escape 102 | as 100 (identical bitstream); differs only in the frame header | **implemented** — native decoder (`replay_esc100`) |
| 122 | Escape 122 (**PAL8**) | palettised 8-bit; 8×8 superblocks of 2×2 macroblocks; inline VGA palette; delta-coded | **implemented** — native decoder (`replay_esc122`) |
| 124 | Escape 124 (RGB555) | RGB555 8×8 superblocks of 2×2 macroblocks; three rotating codebooks; skip VLC; mask + pattern placement | **implemented** — native decoder (`replay_esc124`) |
| 130 | Escape 2.0 | YCbCr 2×2-block grid, delta/skip-coded, 2× upsampled render | **implemented** — native decoder (`replay_esc130`) + bit-exact render |

> **122 is *not* escape124.** Despite the shared "Escape" name and an earlier
> assumption here, codec 122 is a **completely different, palettised (PAL8)**
> format — the proprietary Windows Streamer DLLs (`WINSDEC`/`EDEC` for 124,
> `DEC130` for 130) cannot decode it; only the original Eidos **DOS** player did.
> Running 122 data through `WINSDEC`'s 124 bit-parser yields garbage. It is
> decoded by its own in-tree decoder ([§ Type 122](#type-122--palettised-pal8)).

This spec records the version lineage and the per-revision frame format. All three
later codecs (122, 124, 130) are now decoded by in-tree native decoders; 130 was
previously a NUT pass-through to FFmpeg's `escape130` (see the history note below).

## Version timeline

| Ver | ~Era | Origin | Frame header | Pixel model | Block coding |
| --- | --- | --- | --- | --- | --- |
| **100** | 1993 | Acorn ARMovie | `u32 id=0x100` | 5-bit-Y YUV555, 160×128 | 2×2-block VQ; chroma from a 256-entry codebook; luma = 1–2 5-bit values + a 3-bit selector mask; escalating block-skip VLC (see § Type 100/102) |
| **102** | 1993 | Acorn ARMovie | `u32 id=0x102` + reserved word | as 100 | **identical bitstream** to 100 (the "different luma code" is only the module's field packing; the bits are the same) |
| **122** | mid-90s | Acorn/Eidos | per-chunk `[0x116][vsize][pal]` | **PAL8** | 8×8 superblocks of 2×2 macroblocks; inline VGA palette; per-macroblock 2-colour or uniform index; escalating skip VLC; delta-coded (see § Type 122) |
| **124** | mid-90s | Eidos games / ARMovie | per frame: `flags`(32) + `size`(32); block bitstream **LSB-first** | RGB555 | 8×8 superblocks of 2×2 macroblocks; three rotating codebooks (4-bit mask + two RGB555 colours); escalating skip VLC; mask + pattern placement (FFmpeg `escape124` variant) |
| **130** | 1997 | Eidos games + ARMovie | 16-byte, *skipped* by the decoder | YUV420P | 2×2 blocks; per-block luma via `sign_table`/`offset_table {2,4,10,20}`; chroma via `chroma_adjust`→`chroma_vals` (32 entries); VLC skip codes (skip=0/3/8/15-bit escalating) |

The frame-header evolution is the clearest version marker. The earliest codec
(100) prefixes each frame with **only** a 32-bit word holding the codec ID; 102
adds one reserved word (an 8-byte header); "later revisions decided to add frame
size and even flags that affect [the] decoding process" (Kostya, below). By
122/130 the header is 16 bytes of
little-endian words, and by the games' Escape 124 it is the 8-byte
`flags`+`size` pair FFmpeg reads MSB-first through its bit-reader.

## What we observe in the samples

All 130 samples are 320×240, 25 fps, one frame per chunk, author `"ESCAPE 2.0"`,
`Copyright (c) 1997 Eidos plc`. The 122 sample (`tank.rpl`) is 320×200, author
`"Eidos"`.

**Type 130** — first 16 bytes of two `Pumpkin.rpl` frames:

```
frame 0 (keyframe): 30 01 01 80 | 60 35 00 00 | 00 00 00 00 00 00 00 00
frame 1 (inter):    30 01 01 00 | f4 0a 00 00 | 00 00 00 00 00 00 00 00
                    \_________/   \_________/
                    flags/ver     u32 LE = frame size (0x3560=13664, 0x0af4=2804)
```

- word `+4` = the frame's payload length (matches the catalogue's `video=` bytes);
- byte `+3` = `0x80` on the keyframe, `0x00` on the inter frame — a key-frame flag;
- the real Escape bitstream begins at `+16` (`c5 21 8c 88 …`).

This 16-byte header is **exactly the 16 bytes FFmpeg's `escape130` skips** before
reading the bitstream, so the whole payload — header and all — is what its decoder
expects.

**Type 122** — the first bytes of a `tank.rpl` frame (`16 01 00 00 | 10 00 00 00 |
03 00 00 00 | …`) are the **Escape 122** per-chunk header, *not* an escape124
`flags`/`size` pair: `+0` is the magic `0x116`, `+4` the chunk video size, `+8` a
16-bit palette length. The bitstream is palettised (PAL8) and unrelated to
escape124 — see [§ Type 122](#type-122--palettised-pal8).

## All three later codecs are decoded natively

Each of 122, 124 and 130 has its own in-tree decoder:

- **122** → `replay_esc122` (PAL8) — no FFmpeg or DLL decoder exists for it at all.
- **124** → `replay_esc124` (RGB555) — the ARMovie variant differs from FFmpeg's
  `escape124` (LSB-first, a swapped transition table, a 17-bit mask+continue field
  and a pattern path), and has no NUT FourCC to pass through under anyway.
- **130** → `replay_esc130` — a clean-room bitstream decoder plus a bit-exact
  reimplementation of `DEC130.DLL`'s render ([§ Type 130](#type-130--ycbcr-2x2-block-codec)).

> **History.** 130 was originally a NUT pass-through to FFmpeg's `escape130`
> (`MKTAG('E','1','3','0')` in `libavformat/riff.c`). That worked, but FFmpeg's
> `escape130` render is not the DLL's — the DLL's colour path is genuinely
> non-linear and 2× upsamples with a separable blend. The native decoder reproduces
> `DEC130.DLL` bit-for-bit (decode *and* render), so it replaced the pass-through.

## Type 100/102 — 5-bit-YUV 2×2-block VQ

Codecs 100 and 102 (© Eidos plc 1993) are the earliest Escape codecs, shipped as
the Acorn ARMovie decompressor modules `Decomp100` / `Decomp102`. They are decoded
natively by `src/replay_escape100.c` (`include/replay/replay_escape100.h`) via
`replay-transcode`'s `direct_esc100` dispatch. **100 and 102 are the same codec on
the wire; they differ only in the per-frame header.**

Pixels are **YUV555** words (`Y[0:4] U[5:9] V[10:14]`, one `uint16` per pixel).
The 160×128 picture is an **80×64 grid of 2×2-pixel blocks** and persists across
frames (delta-coded).

### Per-frame layout

A single ARMovie video chunk concatenates several frames. Each frame is:

```
100:  u32 id (== 0x100)                       then the bitstream
102:  u32 id (== 0x102), u32 reserved         then the bitstream
```

The bitstream is **LSB-first**, read as 32-bit little-endian words; frames are
word-aligned (a frame ends on a word boundary, so the next frame's id follows).

### Decode loop

The picture is walked in block raster order with a cursor `(bx,by)`:

```
bx = by = 0
loop:
    bx += read_skip()                 # escalating skip VLC (below); skipped blocks
    while bx >= 80: bx -= 80; by++    #   keep the previous frame's pixels
    if by >= 64: done
    if read_bit() == 1: decode_luma_block(bx, by)
    else:               decode_chroma_block(bx, by)
    bx += 1

read_skip():                          # blocks to copy from the previous frame
    if bit() == 0: return 0
    v = bits(3);  if v != 7:   return 1 + v
    v = bits(7);  if v != 127: return 8 + v
    return 135 + bits(15)

read_chroma_index():                  # index into the 256-entry chroma codebook
    i = bits(6); if i > 48: i += bits(2) << 6; return i
```

### Blocks

A block's four pixels are `chroma | luma5`, where `chroma` is a codebook entry (a
YUV555 value with Y=0) and `luma5` is a 5-bit Y.

- **Luma block** (mode bit 1): `sel = bits(3)`, `lumaA = bits(5)`, and if `sel != 0`
  also `lumaB = bits(5)`. Then `new_chroma = bit()`: if 0, reuse the block's
  existing chroma (its current top-left pixel, luma cleared); if 1, `chroma =
  codebook[read_chroma_index()]`. The 3-bit selector is a per-sub-pixel mask —
  bit0→top-right, bit1→bottom-left, bit2→bottom-right each choose luma B, else A;
  **the top-left pixel is always luma A**.
- **Chroma block** (mode bit 0): `chroma = codebook[read_chroma_index()]`; every
  pixel keeps its own existing luma and takes the new chroma.

  > On 102 the module reads `sel`+`lumaA` as a single `bits(8)` split into low-3 /
  > high-5, which — LSB-first — is the same bits as 100's `bits(3)`+`bits(5)`. So
  > the luma coding is identical; only the frame header differs.

The 256-entry chroma codebook is a static table baked into the modules (identical
in both); the in-tree decoder embeds it. Validated **byte-exact** against the
`Decomp100`/`Decomp102` modules: the real `SplashBox` movie (160×128, 25 frames, ©
Computer Concepts 1993) decodes identically to `Decomp100` frame-for-frame, and a
differential test (`test_escape100_module`) checks both modules on synthetic frames
covering every path.

## Type 122 — palettised (PAL8)

Codec 122 (named `122  video format` in the `.RPL` text header) is a **palettised
8-bit** delta codec, decoded by `src/replay_escape122.c`
(`include/replay/replay_escape122.h`), driven by `replay-transcode`'s
`direct_esc122` dispatch. The on-the-wire magic is `0x116` (124 is `0x114`, 130 is
`0x130`, 102 is `0x102`).

### Per-chunk layout

Each ARMovie video chunk is one frame:

```
u32le  codec_id    == 0x116
u32le  vsize       chunk video size
u16le  pal_size    bytes of palette data that follow
byte[pal_size]     VGA palette, 3 bytes/entry, 6-bit components, expanded 6→8 bits
                   as v = (v << 2) | (v >> 4). min(pal_size/3, 256) entries used;
                   pal_size == 0 ⇒ keep the previous frame's palette.
<bitstream>        LSB-first bit reader
```

For `tank.rpl` the first frames are 16-byte placeholders; the **first real frame
carries the full 768-byte palette**, and most later frames have `pal_size == 0`
and reuse it. The frame buffer **and** palette persist across frames (122 is
delta-coded — skipped superblocks keep their previous contents).

### Image / bitstream

The frame is `width × height` 8-bit palette indices, decoded in **8×8
superblocks** in raster order (strips of 8 rows, then 8-pixel columns). Each
superblock is a **4×4 grid of 2×2 macroblocks**, with macroblock `i`'s top-left
pixel at `offsets[i] = (i & 3)·2 + (i >> 2)·2·width`.

```
per superblock:
    skip = read_ecode()                 # skip-run VLC (below)
    if skip > 0: { skip--; keep previous frame's pixels; next superblock }
    # pass A — broadcast one block into every macroblock whose mask bit is set
    while read_bit() == 0:
        blk  = read_blk2x2()
        mask = read(16); for i in 0..15: if mask bit i: write blk at offsets[i]
    # pass B — a fresh block per masked macroblock
    if read_bit() == 0:
        mask = read(16); for i in 0..15: if mask bit i: write read_blk2x2() at i

read_ecode():                           # skip N superblocks
    if read_bit() == 0: return 0
    v3 = read(3);  if v3 != 7:   return v3 + 1
    v7 = read(7);  if v7 != 127: return v7 + 1 + 7
    return read(12) + 1 + 7 + 127

read_blk2x2() -> px[0..3] = TL,TR,BL,BR:
    m4 = read(4)
    m4 == 0x0 -> idx = read(7);  all four = idx*2          # uniform, even index
    m4 == 0xF -> idx = read(7);  all four = idx*2 + 1      # uniform, odd index
    else      -> c0 = read(8); c1 = read(8); px[k] = (m4>>k)&1 ? c1 : c0
```

Output is PAL8: index → `palette[idx·3 + 0..2]` as 8-bit R,G,B. Validated end to
end on `tank.rpl` (320×200, 1925 frames: an ocean-sunrise title sequence and
attack-helicopter gameplay); `replay-transcode`'s output is byte-identical to the
standalone reference decoder, and a unit test (`test_replay_escape122`) covers a
hand-built one-superblock frame.

## Type 124 — RGB555 block codec

Codec 124 is the Eidos games "Escape" codec decoded by `WINSDEC.DLL` (`SC_Frame`)
and `EDEC.DLL` (`EC_Frame`), decoded natively by `src/replay_escape124.c`
(`include/replay/replay_escape124.h`) via `replay-transcode`'s `direct_esc124`
dispatch. Its internal pixel format is **RGB555** (`0RRRRRGGGGGBBBBB`, red high),
one `uint16` per pixel. It is *not* 122 (PAL8); the two share only the
superblock/skip-VLC skeleton.

### Frames per chunk

Unlike 122/130 (one frame per chunk), a real codec-124 movie packs the
header's **"frames per chunk"** count into one ARMovie video chunk, each frame a
self-delimiting unit:

```
u32le  flags       escape124 frame flags (mode gate + codebook-resend bits)
u32le  size         total bytes of THIS frame, including this 8-byte header
<bitstream>         LSB-first block bitstream
```

The transcoder walks a chunk by advancing `size` bytes per frame until the chunk's
video bytes are consumed. Inter-frame deltas reference the immediately preceding
frame, so every frame is decoded in order.

### Frame model

The frame is a grid of **8×8 superblocks**, each a 4×4 grid of **2×2 macroblocks**
(four RGB555 pixels). The decoder keeps three **codebooks** of macroblocks and a
persistent RGB555 frame; skipped superblocks keep their previous contents.

```
flags gate:   if (flags & 0x7800000) == 0  ⇒  whole frame copied from previous
codebooks:    for i in 0,1,2 if flags & (1<<(17+i)) resend codebook (below)
per superblock (raster order), current codebook index persists across the frame:
    skip = skip_vlc()                       # escalating 1/+3/+7/+12-bit code
    if skip > 0: { skip--; keep previous superblock; next }
    leading = read_bit()                    # 1 ⇒ straight to the pattern path
    if !leading:                            # main loop
        repeat:
            mb   = read_macroblock()        # codebook-switch bit, then depth-bit index
            m17  = read(17); mask = m17 & 0xffff; continue = (m17 >> 16) & 1
            for k in 0..15: if mask & mask_matrix[k]: place mb at slot k
            if continue: break              # drop into the pattern path
    pattern path: sub = read_bit()
        sub == 1 and (flags & 0x10000):     while !read_bit(): place read_macroblock() at read(4)
        sub == 0:  sel = read(4); ebp = accumulated_mask ^ pattern_delta(sel)
                   for k in 0..15: if ebp & mask_matrix[k]: place read_macroblock() at slot k

codebook resend:
    i==0 → slot 1, depth = read(4),  size = 1<<depth
    i==1 → slot 0, depth = read(4),  size = num_superblocks<<depth   (per-superblock)
    i==2 → slot 2, size = read(20),  depth = ilog2(size-1)+1
    each entry (34 bits): mask = read(4); c0 = read(15); c1 = read(15);
                          pixel[j] = (mask>>j)&1 ? c1 : c0
read_macroblock: if read_bit(): cb = transitions[cb][read_bit()]
                 idx = read(cb.depth); if cb==0: idx += superblock_index<<cb.depth
                 return cb.blocks[idx]
```

`mask_matrix` maps the row-major 4×4 slot `k` to its bit in the 16-bit mask;
`transitions[3][2] = {{1,2},{2,0},{0,1}}` (the ARMovie column order). The decoder
also reproduces a genuine WINSDEC bit-reader quirk — a stale dword look-ahead
register across 32-bit boundaries — so the output matches the shipping decoder
bit-for-bit.

Validated on `ESCAPE.RPL` (320×240, 4×25 frames) and `PYRAMID.RPL` (320×120,
15/chunk): `replay_esc124`'s RGB555 is byte-identical to the standalone reference
decoder every frame, and `replay-transcode`'s RGB24 output is byte-identical to
**FFmpeg's own `escape124`** decoder (an independent implementation) across the
whole video. A unit test (`test_replay_escape124`) covers a hand-built
one-superblock frame.

## Type 130 — YCbCr 2×2-block codec

Codec 130 ("Escape 2.0") is decoded natively by `src/replay_escape130.c`
(`include/replay/replay_escape130.h`) via `replay-transcode`'s `direct_esc130`
dispatch, with the render in `src/dec130_render.c`. One frame per chunk.

### Per-chunk layout

```
u16le  magic       == 0x130
u16le  flags       (not needed to decode pixels)
u32le  vsize       total video size of this chunk
8 bytes            reserved
<bitstream>        LSB-first, vsize-16 bytes
```

A chunk with fewer than 16 bytes carries no bitstream and is a **"no change"**
frame (identical to the previous one).

### Picture model and decode

The picture is a grid of **2×2-pixel blocks** (`BW = W/2` by `BH = H/2`), each
holding a packed 32-bit word: a 6-bit base luma, a 5+5-bit chroma pair, and four
2-bit per-sub-pixel "texture" signs. The block-state array **persists across
frames** and is delta-coded; an escalating skip-run VLC copies unmentioned blocks
from the previous frame.

```
i = read_skip()                          # initial skip (§5 VLC)
while i < BW*BH:
    pred = (i>0) ? blk[i-1] : SEED0      # predictor = left neighbour, wraps rows
    blk[i] = decode_block(pred)          # luma mode then chroma mode (below)
    i += 1 + read_skip()

luma mode  : 1 -> SIGNS(sidx6,step2,ya5); 00 -> COPY; 010 -> DELTA(d3); 011 -> ABS(v6)
chroma mode: 0 -> COPY; 10 -> DELTA(d3); 11 -> ABS(cb5,cr5)
read_skip():  bit==1 -> 0; else v=bits(3) (v?v); v=bits(8) (v?v+7); bits(15)+262
```

SIGNS writes a fresh texture pattern (a 64-entry sign table) and marks the block
*textured*; COPY+COPY copies the predictor verbatim (texture included) but renders
flat. The full block-word layout, the `Y_DIFF`/`C_DIFF`/sign tables, and the packed
chroma add are in the decoder and `docs/spec` behavioural spec.

### Render

`DEC130.DLL` treats the block grid as a low-resolution colour grid and **2×
upsamples** it with a separable (2,1,1)/4 blend, rendering textured blocks sharply
(each sub-pixel gets a ±luma step) and packing RGB565; the chroma response is
non-linear (three DLL colour tables). `src/dec130_render.c` is a hand-written
reimplementation of that path, **bit-exact** to the DLL, expanded here to RGB888.

Validated on all seven 320×240 samples (`Pumpkin`, `Victory`, `cam_start`,
`inflight`, `landing`, `noVideo`, `win`) plus other games' 130 movies: the native
decoder's RGB output is **byte-identical to the standalone reference decoder**
(itself bit-exact to `DEC130.DLL`) on every sampled frame. A unit test
(`test_replay_escape130`) covers a hand-built one-block frame.

**Sound.** Escape 2.0 movies carry ARMovie sound format 1 (16-bit linear) or
format **101**, the Eidos "Escape"/WINSTR 4-bit ADPCM. Both are decoded and muxed,
so type-130 movies (`Victory`, `inflight`, …) transcode with sound. Format 101 is
*not* the linear PCM its "LINEAR UNSIGNED" label claims — it is a non-canonical IMA
ADPCM; see [armovie-sound.md §4](armovie-sound.md). (`tank` is the one 8-bit-101
sample; its video is now decoded by the native Escape **122** path above, and its
8-bit-101 audio uses the linear-PCM branch — unconfirmed, no reference.)

## Appendix — provenance

- **Type 100/102 (Escape 100/102)** were reverse-engineered directly from the
  vendored `Decomp100` / `Decomp102` ARM modules (disassembly + tracing under the
  in-tree ARMulator), cross-checked with Kostya's blog below. The [§ Type 100/102]
  (#type-100102--5-bit-yuv-2x2-block-vq) format above describes the bitstream;
  `src/replay_escape100.c` is a clean, readable reimplementation of it (not a
  register transliteration), with the modules' 256-entry chroma codebook embedded
  verbatim. Validated **byte-exact** against those modules: the real `SplashBox`
  movie for 100, and differential path-covering synthetic frames for both.
- **Kostya / Nihav**, *"A quick look at Eidos Escape codecs"*,
  <https://codecs.multimedia.cx/2024/04/a-quick-look-at-eidos-escape-codecs/>
  (2024-04): background for the Escape 100/102 reverse engineering — 160×128,
  5-bit YUV, 2×2 VQ, the chroma codebook, the luma value+mask coding, and the note
  that the skip code "survived until Escape 124" and the codecs "survived for some
  time in the games."
- **FFmpeg** `libavcodec/escape124.c`, `libavcodec/escape130.c`: the block-level
  bitstreams for the later codecs — escape124's `flags`+`size` header, 8×8
  superblocks / 2×2 macroblocks / three codebooks / RGB555 output; escape130's
  16-byte skipped header, 2×2 blocks, `sign_table`/`offset_table {2,4,10,20}`,
  `chroma_adjust`/`chroma_vals`, escalating skip codes, and `yuv420p` output.
- **FFmpeg** `libavformat/riff.c`: `{ AV_CODEC_ID_ESCAPE130, MKTAG('E','1','3','0') }`
  — the only Escape container tag (escape124 has none). This is what the earlier
  130 NUT pass-through relied on, before the native `replay_esc130` decoder replaced
  it; FFmpeg's `escape130` render does not match `DEC130.DLL` bit-for-bit.
- **Type 122 (Escape 122)** was reverse-engineered from the Eidos **DOS** player
  (the Windows Streamer DLLs only decode 124/130 — `WINSDEC`/`EDEC` = 124,
  `DEC130` = 130). The [§ Type 122](#type-122--palettised-pal8) format above is a
  **behavioural specification**; `src/replay_escape122.c` is a **clean-room**
  implementation written strictly from that spec (no existing decoder consulted),
  validated byte-for-byte against an independent standalone reference decoder on
  `tank.rpl`.
- **Type 124 (Escape 124)** was reverse-engineered from `WINSDEC.DLL` (`SC_Frame`)
  and cross-referenced with the publicly documented **FFmpeg `escape124`** algorithm
  (the `0x7800000` gate, `mask_matrix`, transition table, codebook-flag bits, skip
  VLC); the ARMovie-specific parts (LSB-first order, swapped transitions, the 17-bit
  mask+continue field, the pattern path, and a WINSDEC dword-lookahead quirk) came
  from the RE. `src/replay_escape124.c` is a reimplementation of that algorithm
  ([§ Type 124](#type-124--rgb555-block-codec) above), validated byte-for-byte
  against both a standalone reference decoder and FFmpeg's `escape124` on
  `ESCAPE.RPL` / `PYRAMID.RPL`.
- **Type 130 (Escape 2.0)** has two provenances. The bitstream **decoder**
  (`src/replay_escape130.c`) is a **clean-room** implementation from the behavioural
  spec ([§ Type 130](#type-130--ycbcr-2x2-block-codec)); no existing decoder was
  consulted. The **render** (`src/dec130_render.c`) is a hand-written reimplementation
  of `DEC130.DLL`'s display path, reverse-engineered from the DLL and bit-exact to it.
  Together they are byte-identical to a standalone reference decoder (itself bit-exact
  to `DEC130.DLL`) on every 130 test movie.
- **Direct byte inspection** of `test-videos/mplayer-samples/{Pumpkin,Victory,
  cam_start,inflight,landing,noVideo,win}.rpl` (130) and `tank.rpl` (122): the
  16-byte LE frame headers, the size-at-`+4` and keyframe-flag-at-`+3` fields, and
  the end-to-end FFmpeg decode of the `"E130"`-tagged NUT output.
