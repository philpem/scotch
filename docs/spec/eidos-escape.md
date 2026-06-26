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
| 122 | Escape (RGB555 superblock) | RGB555, 8×8 superblocks, 3 codebooks | **not yet** — needs a native decoder (≈ FFmpeg `escape124`) |
| 130 | Escape 2.0 | YUV420P, 2×2 blocks, skip-coded | **implemented** — NUT pass-through to FFmpeg `escape130` |

This spec records the version lineage, the per-revision frame format, and *why*
130 is a pass-through while 122 is not. The block-level bitstreams of 122/124 and
130 are FFmpeg's (`libavcodec/escape124.c`, `escape130.c`); this document
captures the **ARMovie-specific** framing and the mapping decisions, which is what
the transcoder needs.

## Version timeline

| Ver | ~Era | Origin | Frame header | Pixel model | Block coding |
| --- | --- | --- | --- | --- | --- |
| **100** | 1993 | Acorn ARMovie | 32-bit codec ID only | 5-bit YUV420, 160×128 | 2×2 two-colour VQ; chroma from a 256-entry combined U/V codebook; luma = two 5-bit values + 3-bit selector mask; variable-length block-skip codes |
| **102** | 1993 | Acorn ARMovie | 32-bit codec ID only | as 100 | as 100; *only the luma code differs* |
| **122** | mid-90s | Acorn/Eidos | 16-byte (adds size + flags) | RGB555 | 8×8 superblocks of 2×2 macroblocks; three rotating codebooks; the same VLC skip code that "survived until Escape 124" |
| **124** | mid-90s | Eidos games | 8-byte: `flags`(32) + `size`(32), MSB-first | RGB555 | as 122 (FFmpeg `escape124`) |
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

**Type 122** — first 16 bytes of a `tank.rpl` frame are the same shape
(`16 01 00 00 | 10 00 00 00 | 03 00 00 00 | …`, little-endian, size at `+4`), but
this is **not** the byte layout FFmpeg's `escape124` reads (it expects an 8-byte
MSB-first `flags`+`size`), and the games' RGB555 superblock bitstream differs from
130's YUV.

## Why 130 is a pass-through and 122 is not

The transcoder's pass-through path muxes codec frames into NUT under a FourCC and
lets FFmpeg decode them (see [../moviefs-nut-passthrough.md](../moviefs-nut-passthrough.md)).
It only works if FFmpeg can *recognise* the FourCC:

- `escape130` **has** a container tag — `MKTAG('E','1','3','0')` in
  `libavformat/riff.c` — so a NUT stream tagged `"E130"` is decoded by FFmpeg's
  `escape130`. ✔
- `escape124` has a decoder but **no** RIFF/NUT/MOV tag, so there is no FourCC to
  put in the container; FFmpeg cannot be told a stream is escape124. ✘

So **130 → pass-through** and **122 → would need a native in-tree decoder** (a
clean-room RGB555 superblock decoder, the FFmpeg `escape124` algorithm), or a
`Decomp122` ARM module (none is present in any vendored tree — we hold only
`Decomp100`/`Decomp102`).

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
sample but its video is Escape 124, which is not yet decoded — see #42.)

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
  pass-through possible and 122 not.
- **Direct byte inspection** of `test-videos/mplayer-samples/{Pumpkin,Victory,
  cam_start,inflight,landing,noVideo,win}.rpl` (130) and `tank.rpl` (122): the
  16-byte LE frame headers, the size-at-`+4` and keyframe-flag-at-`+3` fields, and
  the end-to-end FFmpeg decode of the `"E130"`-tagged NUT output.
