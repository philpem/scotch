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
| 100 | Escape 100 | 160×128, 5-bit YUV420, 2×2 VQ | `Decomp100` ARM module vendored, not wired |
| 102 | Escape 102 | as 100, different luma code | `Decomp102` ARM module vendored, not wired |
| 122 | Escape 122 (**PAL8**) | palettised 8-bit; 8×8 superblocks of 2×2 macroblocks; inline VGA palette; delta-coded | **implemented** — native decoder (`replay_esc122`) |
| 124 | Escape 124 (RGB555) | RGB555 8×8 superblocks of 2×2 macroblocks; three rotating codebooks; skip VLC; mask + pattern placement | **implemented** — native decoder (`replay_esc124`) |
| 130 | Escape 2.0 | YUV420P, 2×2 blocks, skip-coded | **implemented** — NUT pass-through to FFmpeg `escape130` |

> **122 is *not* escape124.** Despite the shared "Escape" name and an earlier
> assumption here, codec 122 is a **completely different, palettised (PAL8)**
> format — the proprietary Windows Streamer DLLs (`WINSDEC`/`EDEC` for 124,
> `DEC130` for 130) cannot decode it; only the original Eidos **DOS** player did.
> Running 122 data through `WINSDEC`'s 124 bit-parser yields garbage. It is
> decoded by its own in-tree decoder ([§ Type 122](#type-122--palettised-pal8)).

This spec records the version lineage, the per-revision frame format, and *why*
130 is a pass-through while 122 and 124 are not.

## Version timeline

| Ver | ~Era | Origin | Frame header | Pixel model | Block coding |
| --- | --- | --- | --- | --- | --- |
| **100** | 1993 | Acorn ARMovie | 32-bit codec ID only | 5-bit YUV420, 160×128 | 2×2 two-colour VQ; chroma from a 256-entry combined U/V codebook; luma = two 5-bit values + 3-bit selector mask; variable-length block-skip codes |
| **102** | 1993 | Acorn ARMovie | 32-bit codec ID only | as 100 | as 100; *only the luma code differs* |
| **122** | mid-90s | Acorn/Eidos | per-chunk `[0x116][vsize][pal]` | **PAL8** | 8×8 superblocks of 2×2 macroblocks; inline VGA palette; per-macroblock 2-colour or uniform index; escalating skip VLC; delta-coded (see § Type 122) |
| **124** | mid-90s | Eidos games / ARMovie | per frame: `flags`(32) + `size`(32); block bitstream **LSB-first** | RGB555 | 8×8 superblocks of 2×2 macroblocks; three rotating codebooks (4-bit mask + two RGB555 colours); escalating skip VLC; mask + pattern placement (FFmpeg `escape124` variant) |
| **130** | 1997 | Eidos games + ARMovie | 16-byte, *skipped* by the decoder | YUV420P | 2×2 blocks; per-block luma via `sign_table`/`offset_table {2,4,10,20}`; chroma via `chroma_adjust`→`chroma_vals` (32 entries); VLC skip codes (skip=0/3/8/15-bit escalating) |

The frame-header evolution is the clearest version marker. The earliest codecs
(100/102) prefix each frame with **only** a 32-bit word holding the codec ID;
"later revisions decided to add frame size and even flags that affect [the]
decoding process" (Kostya, below). By 122/130 the header is 16 bytes of
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

## Why 130 is a pass-through (and 122/124 are not)

The transcoder's pass-through path muxes codec frames into NUT under a FourCC and
lets FFmpeg decode them (see [../moviefs-nut-passthrough.md](../moviefs-nut-passthrough.md)).
It only works if FFmpeg can *recognise* the FourCC:

- `escape130` **has** a container tag — `MKTAG('E','1','3','0')` in
  `libavformat/riff.c` — so a NUT stream tagged `"E130"` is decoded by FFmpeg's
  `escape130`. ✔
- `escape124` has **no** RIFF/NUT/MOV tag, so no FourCC to pass through under; and
  the ARMovie 124 variant differs from FFmpeg's `escape124` anyway (LSB-first, a
  swapped transition table, an extra 17-bit mask+continue field and a pattern
  path). So 124 gets a **native decoder** (`replay_esc124`, the RGB555 format below). ✘
- **122 has no FFmpeg (or DLL) decoder at all** — it is the PAL8 format below.

So **130 → pass-through**, and **122 / 124 → native decoders** (`replay_esc122`
PAL8, `replay_esc124` RGB555).

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

## Implementation (type 130)

`replay-transcode` case 130 maps to FourCC `"E130"` with wrap kind
`REPLAY_WRAP_NONE`: each chunk holds exactly one frame and carries no MovieFS-style
per-frame wrapper, so the **whole chunk payload** (16-byte Escape header included)
is muxed as one NUT packet. FFmpeg's `escape130` skips the header and decodes the
rest to `yuv420p`. Requires `--output-format nut` (the default), like the other
pass-through codecs.

```sh
build/replay-transcode --input Pumpkin.rpl \
  | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

Validated end to end on all seven 130 samples (`Pumpkin`, `Victory`, `cam_start`,
`inflight`, `landing`, `noVideo`, `win`): FFmpeg auto-detects
`escape130 (E130 / 0x30333145)` and decodes every frame with no errors, producing
the correct images.

**Sound.** Escape 2.0 movies carry ARMovie sound format 1 (16-bit linear) or
format **101**, the Eidos "Escape"/WINSTR 4-bit ADPCM. Both are decoded and muxed,
so type-130 movies (`Victory`, `inflight`, …) transcode with sound. Format 101 is
*not* the linear PCM its "LINEAR UNSIGNED" label claims — it is a non-canonical IMA
ADPCM; see [armovie-sound.md §4](armovie-sound.md). (`tank` is the one 8-bit-101
sample; its video is now decoded by the native Escape **122** path above, and its
8-bit-101 audio uses the linear-PCM branch — unconfirmed, no reference.)

## Appendix — provenance

- **Kostya / Nihav**, *"A quick look at Eidos Escape codecs"*,
  <https://codecs.multimedia.cx/2024/04/a-quick-look-at-eidos-escape-codecs/>
  (2024-04): the ARMovie Escape 100/102 reverse engineering — 160×128, 5-bit
  YUV420, 2×2 VQ, the chroma codebook, the luma value+mask coding, the 32-bit
  codec-ID frame header, and the note that the skip code "survived until Escape
  124" and that the codecs "survived for some time in the games."
- **FFmpeg** `libavcodec/escape124.c`, `libavcodec/escape130.c`: the block-level
  bitstreams for the later codecs — escape124's `flags`+`size` header, 8×8
  superblocks / 2×2 macroblocks / three codebooks / RGB555 output; escape130's
  16-byte skipped header, 2×2 blocks, `sign_table`/`offset_table {2,4,10,20}`,
  `chroma_adjust`/`chroma_vals`, escalating skip codes, and `yuv420p` output.
- **FFmpeg** `libavformat/riff.c`: `{ AV_CODEC_ID_ESCAPE130, MKTAG('E','1','3','0') }`
  — the only Escape container tag (escape124 has none), which is what makes 130
  pass-through possible and 124 not.
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
- **Direct byte inspection** of `test-videos/mplayer-samples/{Pumpkin,Victory,
  cam_start,inflight,landing,noVideo,win}.rpl` (130) and `tank.rpl` (122): the
  16-byte LE frame headers, the size-at-`+4` and keyframe-flag-at-`+3` fields, and
  the end-to-end FFmpeg decode of the `"E130"`-tagged NUT output.
