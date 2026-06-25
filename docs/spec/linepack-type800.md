# Replay type 800 — LinePack

Replay/ARMovie video format **800** is **LinePack**, a third-party temporal/spatial
codec by *Henrik Bjerregaard Pedersen* (1995). Status: *implemented — decoded by
its vendored `Decomp800/Decompress` module under the CodecIf/ARMulator harness
(`replay-transcode` case 800). Validated end-to-end on `TEKTRAILER.rpl` (160×120,
475 frames, exact byte consumption, correct images).*

## Identity

`Decomp800/Info` declares:

```
LinePack
© Henrik Bjerregaard Pedersen, 1995

4;32;1024
4;32;1024
Temporal,Spatial
YUV 5,5,5; RGB 5,5,5
```

Sample: `test-videos/samples.mplayerhq.hu-archive-container-rpl/rpl+0x0320+pcm_s16le++TEKTRAILER.rpl`
— "Trailer for TEK 1608", © Artex Software 1998, 160×120, 12.5 fps, header colour
"16 bits per pixel (RGB)".

## Decode dimensions — the key requirement

**LinePack decodes at the *exact* declared frame size; it must not be
block-rounded.** The Info's middle dimension field (`32`) is an *alignment hint*,
not a frame-padding requirement: TEKTRAILER is 160×**120**, and 120 is not a
multiple of 32. Decoding at the block-rounded 160×128 makes the decoder fill
20480 words per frame from a stream that only encodes 19200, so it over-reads the
source; the per-frame source pointer drifts and, after ~22 frames, runs past the
chunk ("decompressor consumed an invalid range"). At the exact 160×120 the 25
frames of a chunk consume the chunk's video bytes precisely (35416/35416) and every
frame decodes correctly.

The transcoder marks the codec `exact_size` (`codec_info` case 800) so
`transcode_video` skips the Info-derived block rounding for it.

## Bitstream (reverse-engineered from `Decomp800/Decompress`)

The decoder fills the output frame left-to-right, top-to-bottom, one 32-bit word
per pixel, reading **16-bit code units** from the source (a word load with a
2-byte post-increment — the classic ARM unaligned-LDR-rotate idiom — masked to 16
bits). For each unit:

- **bit 15 clear → literal pixel.** The low 15 bits are the pixel value; they pass
  through `FNplook` and are stored as a word. `FNplook` is the unpatched
  passthrough form (`MOV r5,r5`), so the stored word is the raw 15-bit value — a
  `RGB 5,5,5` (or `YUV 5,5,5`) sample. The colour model comes from the movie
  header (TEKTRAILER: RGB → `COL_RGB555`).
- **bit 15 set → escape.** Bits 12–14 select one of eight ops via a jump table:

  | op | meaning |
  | -- | ------- |
  | 0 | **temporal copy** — copy `count` (bits 0–11) pixels from the previous frame at the same position |
  | 1 | **motion copy** — signed MV `hmv=(bits0–2)−4`, `vmv=(bits3–5)−4`, `count=bits6–11`; copy from the previous frame at `offset + (width·vmv + hmv)`, **or**, when MV=(0,0), from one row up in the *current* frame (spatial) |
  | 2 | **run-length** — read one pixel, replicate it `count` times |
  | 3 | **long temporal copy** — `count` is a full 16-bit word read from the source, then copy that many pixels from the previous frame |
  | 4 | **literal block** — read a short run of literal pixels |
  | 5 | **literal pair run** — read a pixel pair and replicate |
  | 6, 7 | **end of frame** (return) |

The frame ends when the output pointer reaches `width·height` words (or an
explicit op-6/7). Every colour-lookup site in the module (the literal store and
the block ops) is an unpatched `MOV rX,rX` passthrough, so **none of the colour
lookups affect control flow or source consumption** — patching a colour table
would change pixel *values* only. The corruption seen before the fix was therefore
purely the dimension/over-read issue above, not a missing colour table.

## Sound

TEKTRAILER carries ARMovie sound format 1 (16-bit signed linear), which the
transcoder already decodes and muxes.

## Appendix — provenance

- `vendor/armovie-codecs/Decomp800/{Info,Decompress,ffd}` — the vendored LinePack
  decompressor; the grammar above is read from its ARM disassembly (frame routine
  at module offset 0x8, escape jump table at 0x4c, patch table at 0x35c).
- The exact-size requirement was established empirically: at 160×120 the decoder
  consumes each chunk's video bytes exactly and produces correct frames, where the
  block-rounded 160×128 over-reads and desyncs. Confirmed on `TEKTRAILER.rpl`
  (issue #45).
- The motion-vector address arithmetic (conditional `cmpeq`/`mlane`/`subne`/`addne`)
  was cross-checked against the ARMulator with a standalone harness and matches.
