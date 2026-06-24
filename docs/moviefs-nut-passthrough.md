# MovieFS (Decomp600–699) → NUT pass-through

Issue #9 asks for transcode support for the video formats MovieFS encapsulates
into Replay containers. These are Warm Silence Software's allocation, codec
numbers **600–699**, and are re-encapsulations of PC (AVI/QuickTime) codecs.
Rather than port the MovieFS ARM decoders, the plan is to lift the encapsulated
codec frames out of the Replay container, mux them into a [NUT](nut-output.md)
stream with the matching FFmpeg fourcc, and let FFmpeg decode them (its decoders
are better tested, and we avoid shipping the MovieFS decoders).

This document records which of those formats FFmpeg can decode *and* can actually
be reached through a NUT stream. The codec names come from
`ARMovie_2003/Docs/Status` (the authoritative MovieFS inventory).

## Method

`replay-transcode` already has a dependency-free NUT muxer
(`src/replay_nut.c`). The muxer is codec-agnostic: a stream carries a fourcc and
optional extradata. To test reachability, a throwaway probe emitted a one-frame
NUT stream per candidate fourcc and `ffprobe`/`ffmpeg` reported what FFmpeg's NUT
*demuxer* maps each tag to, and whether the decoder can initialise. FFmpeg
6.1.1 was used.

Two facts about FFmpeg's NUT demuxer drive the result:

1. It maps a video fourcc through `ff_nut_video_tags` (raw formats) and
   `ff_codec_bmp_tags` (the AVI/BMP table) — **not** the MOV table. QuickTime
   codecs are still reachable only because their fourccs also appear in the BMP
   table (`cvid`, `rpza`, `smc `, `rle `, …).
2. The NUT video stream header carries width, height, sample aspect, colourspace
   and **extradata**, but there is **no `bits_per_coded_sample` field**. Codecs
   whose decoder needs the pixel depth from `bits_per_coded_sample` (the RLE
   family) therefore cannot be carried by NUT as-is and fail to initialise with
   *"0 bits/sample"* / *"unsupported bits per sample"*.

End-to-end remux round-trips (`encode → -c copy into NUT → decode`) were run for
the three codecs FFmpeg can also encode: **Cinepak passes, msvideo1 (16-bit)
passes, qtrle fails** with the bits/sample error — confirming (2).

## Outcome table

Legend — **NUT verdict**:
✅ clean pass-through (fourcc + geometry is enough);
🎨 needs a palette/extradata (carry via NUT extradata, `pal8`);
⛔ needs `bits_per_coded_sample`, which NUT cannot carry → use an AVI/MOV remux;
🟡 raw/uncompressed (route via NUT `rawvideo`, not a "real" decode);
🚫 no usable FFmpeg path.

| Decomp | Status name | PC codec | FFmpeg decoder | NUT fourcc | Tag seen by NUT demux | NUT verdict |
| -----: | ----------- | -------- | -------------- | ---------- | --------------------- | ----------- |
| 600 | CRAM8 AVI | MS Video 1, 8-bit | `msvideo1` | `CRAM`/`MSVC` | ✓ msvideo1 | ⛔/🎨 needs depth=8 + palette (decodes as 16-bit garbage otherwise) |
| 601 | CRAM16 AVI | MS Video 1, 16-bit | `msvideo1` | `CRAM`/`MSVC` | ✓ msvideo1 | ✅ verified end-to-end |
| 602 | CVID AVI/QT | Cinepak | `cinepak` | `cvid` | ✓ cinepak | ✅ verified end-to-end |
| 603 | RPZA QT | Apple Video (RPZA) | `rpza` | `rpza` | ✓ rpza | ✅ (fixed 16-bit, no depth needed) |
| 604 | SMC QT | Apple Graphics (SMC) | `smc` | `smc ` | ✓ smc | 🎨 8-bit palettised; supply palette via extradata |
| 605 | Ultimotion AVI | IBM UltiMotion | `ultimotion` | `ULTI` | ✓ ulti | ✅ (self-contained YUV) |
| 606 | RGB8 AVI | raw 8-bit palettised | `rawvideo` | `PAL\x08` | rawvideo pal8 | 🟡 raw; supply palette via extradata |
| 607 | RLE8 AVI | Microsoft RLE | `msrle` | `MRLE` | ✓ msrle | ⛔ "unsupported bits per sample" |
| 608 | RGB24 AVI | raw BGR24 (DIB) | `rawvideo` | `BGR\x18` | rawvideo bgr24 | 🟡 raw pass-through (mind bottom-up rows) |
| 609 | RLE(8) QT | QuickTime RLE | `qtrle` | `rle ` | ✓ qtrle | ⛔ "0 bits/sample" (verified fail) |
| 610 | FLI/FLC | Autodesk FLIC | `flic` | `FLIC` | ✓ flic | ✅ (palette is in-stream; check FLIC-header extradata) |
| 613 | QT RLE(4) | QuickTime RLE 4-bit | `qtrle` | `rle ` | ✓ qtrle | ⛔ needs depth=4 |
| 614 | QT RLE(16) | QuickTime RLE 16-bit | `qtrle` | `rle ` | ✓ qtrle | ⛔ needs depth=16 |
| 615 | QT RLE(24) | QuickTime RLE 24-bit | `qtrle` | `rle ` | ✓ qtrle | ⛔ needs depth=24 |
| 622 | DL | "DL" animation | — | — | — | 🚫 no FFmpeg decoder |
| 623 | ANM film | Deluxe Paint Animation | `anm` | (none) | not recognised | 🚫 decoder exists but no AVI/BMP/NUT fourcc (FFmpeg reaches it only via its own ANM demuxer) |
| 624 | RGB8 QT | raw 8-bit palettised | `rawvideo` | `PAL\x08` | rawvideo pal8 | 🟡 raw; supply palette via extradata |
| 626 | RGB24 QT | raw RGB24 | `rawvideo` | `RGB\x18` | rawvideo rgb24 | 🟡 raw pass-through |
| 627 | RT13 AVI/QT | unknown (no WSS source) | — | — | — | 🚫 unidentified bitstream |
| 628 | IV31 AVI/QT | Indeo 3.1 | `indeo3` | `IV31` | ✓ indeo3 | ✅ |
| 629 | IV32 AVI/QT | Indeo 3.2 | `indeo3` | `IV32` | ✓ indeo3 | ✅ |
| 630 | QuickTime VR | panoramic VR | — | — | — | 🚫 not a linear video stream |
| 699 | Unknown AVI/QT | unknown | — | — | — | 🚫 unidentified |

`Docs/Status` also lists "no binary" allocations not shipped in MovieFS but worth
noting if they ever turn up: 625 RGB16 (→ rawvideo rgb555/565), 632 Indeo
Interactive R4.1 (→ `indeo4`, tag `IV41`, verified recognised), 633 Photo-JPEG
(→ `mjpeg`, tag `jpeg`, verified recognised), 635 RGB32 (→ rawvideo).

## Tranches for implementation

1. **Easy win — clean NUT pass-through (no extra metadata).**
   602 Cinepak, 601 CRAM16, 603 RPZA, 605 Ultimotion, 628/629 Indeo3, and most
   likely 610 FLI/FLC. This is the bulk of what actually appears (the issue calls
   out type 602 / Cinepak as common) and matches the issue's premise exactly:
   copy the encapsulated frame, set the fourcc, mux to NUT, let FFmpeg decode.

2. **Raw — route through NUT `rawvideo`.**
   608/626 RGB24 (bgr24/rgb24) are trivial. 606/624 RGB8 and 604 SMC need a
   palette, carried as NUT extradata (`pal8`). These don't need FFmpeg's decoder
   at all, but going through the same NUT path keeps one code path.

3. **Depth-parameterised — NUT is insufficient.**
   607 msrle and 609/613/614/615 qtrle (and 600 CRAM8) need
   `bits_per_coded_sample`, which the NUT stream header cannot express. The
   natural carrier is the codec's *original* container — a minimal **AVI**
   (`BITMAPINFOHEADER.biBitCount` + palette) or **MOV** (ImageDescription depth)
   remux — which is, after all, where MovieFS got these frames. Either build a
   tiny AVI/MOV writer for this tranche or extend the muxer plan accordingly.

4. **No path.** 622 DL, 627 RT13, 630 QuickTime VR, 699 Unknown — out of scope.
   623 ANM has an FFmpeg decoder but no container fourcc that NUT/AVI can use, so
   it would need the standalone ANM path or a native decoder.

## Alternative: just run the MovieFS decoder modules

The MovieFS codecs are themselves Replay `Decomp` modules, and `replay-transcode`
already loads `DecompN/Decompress,ffd` and runs it under the ARMulator via
`CodecIf` (types 1–27 today). Source review (`ARMovie_2003/Video/Decomp6xx/bas/
Make*,ffb.txt`, detokenised BASIC with inline ARM assembler) confirms they fit
the same contract: Decomp609 starts with the exact CodecIf three-word header
(`DCD patch_offset / B init / B frame`), and `init`/`frame` are pure ARM with no
runtime SWIs — the only OS calls are build-time `OS_File` saves. Compiled
binaries for every shipping 600–699 codec exist in
`ARMovie_RO2003_compiled/!ARMovie/Decomp6xx`.

So "just use the MovieFS codecs" is mechanically small (a few `case 6xx:` lines +
vendoring the modules) and has big advantages over the NUT path:

- **Coverage.** Handles the depth-parameterised RLE codecs (607/609/613/614/615)
  and CRAM8 (600) that NUT structurally cannot carry, plus DL (622) and ANM
  (623) that FFmpeg-via-NUT can't reach at all.
- **Authenticity.** Bit-exact to the real Acorn player; consistent with this
  project treating the compiled module as ground truth.
- **No FFmpeg runtime dependency.**

But it carries the risks the issue anticipated:

- **Output colour layout is mostly undeclared.** Only Decomp602 (Cinepak) lists
  a working colour (`YUV 5,5,5`, = `REPLAY_PIX_YUV555`, already supported). The
  rest stop at `Temporal`, implying they paint a screen format (RGB/palettised)
  via the colour patch table rather than emitting YUV working words. CodecIf
  currently only knows the YUV/6Y layouts and runs *unpatched*, so each codec's
  real output layout (and whether unpatched operation is even valid) needs
  per-codec source work, possibly a new RGB/pal8 `ReplayPixelLayout` or a
  patch-table fill.
- **Licensing.** WSS (and IMS, for Indeo) binaries. The Acorn codecs are open
  source via ROOL; WSS Replay codecs were freeware, but redistribution should be
  confirmed to the same bar as the already-vendored third-party ones (Eidos,
  Pedersen). 628/629 (Indeo) involve Intel IP and have **no source**, only a
  C-based binary that may not run self-contained under the bare sandbox.

### PoC results (Cinepak / type 602, `IRONMAN.rpl`)

Tested against a real type-602 movie (160×120, 15 fps, "16 bpp yuv";
`test-videos/IRONMAN.rpl`). Two things were established.

**1. The MovieFS encapsulation wraps each codec frame in a 16-byte header.**
Per Replay chunk, every frame is:

```
+0  uint32 LE  size = codec_frame_len + 12
+4  uint32 LE  flags (0)
+8  uint32 LE  width
+12 uint32 LE  height
+16 ...        the raw PC-codec frame (here a verbatim Cinepak frame, whose own
               24-bit big-endian length header gives codec_frame_len)
```

Stride to the next frame is `16 + codec_frame_len`. So the encapsulated data is
the *verbatim* PC codec bitstream — but offset by this wrapper, which the Acorn
codecs (no per-frame wrapper) and hence the CodecIf harness don't expect.

**2. NUT → FFmpeg works; and the native module works too — with the right
variant.** Stripping the wrapper and muxing the raw Cinepak frames into NUT
(`cvid`) decodes correctly in FFmpeg (whole clip, real video out). Separately,
driving the MovieFS module through CodecIf:

- `Decompress` / `DecompresH` (the dithering **screen-painter** variants) **spin
  forever** unpatched: they `STR R3, v_coltab` and index a caller-supplied colour
  table (R3) that ARMovie's Colour subsystem normally provides; with R3 = 0 they
  walk the zero page.
- `Dec24` (and `Dec21`) **work perfectly unpatched**: their `FNplook` is *empty*
  — they do the full YUV→RGB conversion with internal tables and never touch R3.
  Fed the raw Cinepak frame, `Dec24` returned cleanly, consumed exactly the frame
  length, and emitted RGB888 (`00 BB GG RR`) byte-identical content to the FFmpeg
  decode (a near-greyscale clip). All 114 frames decoded natively this way.

So for Cinepak there are **two working paths**, both needing only wrapper
stripping: NUT→FFmpeg, or the native `Dec24` module (bit-exact to Acorn, no
FFmpeg). The earlier "all 602 variants need R3" claim was wrong — it's only the
screen-painter variants.

### Variant inventory (what each codec ships → does it need the Colour subsystem?)

The decompress variants decide whether colour emulation is even needed. Per the
CodecIf doc: `Dec24`/`Dec21` emit the original colour space (full internal
conversion, **no R3**), `Dec16`/`Dec8` emit raw 16-bit / 8-bit-index values
(**no R3**, patch table is just `-1`), while `Decompress`/`DecompresH`/`DecompresB`
are screen painters that use the R3 colour/dither table.

| Codec | r3-free variant available | Native-harness verdict |
| ----- | ------------------------- | ---------------------- |
| 602 Cinepak, 608 RGB24-AVI, 615 QT-RLE24, 626 RGB24-QT | **Dec24/Dec21** → RGB888 | ✅ works (602 validated); strip wrapper, use Dec24 |
| 605 Ultimotion | **Dec16** → 16-bit | ✅ likely; strip wrapper, use Dec16 |
| 600 CRAM8, 604 SMC, 606/624 RGB8, 607 RLE8, 609 QT-RLE8, 610 FLIC, 613 QT-RLE4, 622 DL, 623 ANM | **Dec8** → 8-bit indices | ✅ runnable, but emit palette indices → need the container/codec palette to colourise |
| 601 CRAM16, 603 RPZA, 614 QT-RLE16 | only `Decompress`/`H` | ⚠️ screen-painter only; works *iff* its `plook` is the unpatched-passthrough kind (601's source never stores R3 → promising) — test per codec |
| 628 IV31, 629 IV32, 699 Unknown | only `Decompress` (C-based) | ✗ screen-painter + Intel IP + no source → use FFmpeg |

### What "emulating Replay's colour support" would take — and why it's mostly avoidable

For codecs with an `Dec24`/`Dec21`/`Dec16`/`Dec8` variant (the large majority),
**no colour emulation is needed**: pick that variant, strip the wrapper, and the
existing harness gets a working colour (or palette indices) out. The only piece
to add is a transcoder layout for RGB888-from-Dec24 (the colour enum already has
`COL_RGB888`; CodecIf needs an RGB unpack path) and, for the Dec8 family, plumbing
the palette.

Where a codec ships *only* a screen-painter `Decompress` that hard-uses R3 (worst
case 628/629), emulating colour support means supplying R3 = a pixel colour/dither
lookup table. The mechanism is documented (CodecIf "patch table", opcode 0: each
entry gives the byte offset of an instruction plus dest/source/table register
numbers, terminated by `-1`; the Player rewrites those instructions to do
`LDR dst,[r3, src, LSL #2]`). To get a *working colour* rather than screen pixels
we'd synthesise a pass-through table mapping the codec's intermediate pixel
(typically a computed RGB or a 15-bit YUV index → ~32768 × 4-byte ≈ 128 KB
identity table) to a Replay word. That is real but bounded work, and it is
**only** worth it for the handful of screen-painter-only codecs — and for those
(628/629 Indeo, no source, Intel IP) FFmpeg is the better answer anyway.

### Implemented

`replay-transcode` now supports MovieFS codecs via the native module path
(`tools/replay_transcode.c`, `codec_info` cases + `src/replay_moviefs.c`):

- The 16-byte per-frame wrapper is stripped per chunk by
  `replay_moviefs_unwrap_chunk` (unit-tested in `tests/test_replay_moviefs.c`),
  which also yields the exact frame count so the decoder never runs past a
  chunk's end.
- MovieFS codecs run in **32-bit ARM mode** (`arm_mode_32`) and select the
  `Dec24` variant (`Decomp<N>/Dec24,ffd`) with `COL_RGB888`.
- Wired: **602 Cinepak** (validated end-to-end — 114 frames + audio, frame
  output byte-identical to the ffmpeg NUT path), plus **608/626 RGB24** and
  **615 QT-RLE24** (same harness-compatible `Dec24` variant; not yet sample-
  validated). The MovieFS `Dec24` modules are not vendored — pass
  `--modules-dir` pointing at an ARMovie tree that contains them.

Not yet wired (need work and/or samples): the `Dec8` palette family (600/604/
606/607/609/610/613/622/623/624 — runnable but emit palette indices, so they
need the container/codec palette plumbed through `COL_PAL8`); 605 Ultimotion
(`Dec16`, colour undeclared); the screen-painter-only codecs (601/603/614); and
628/629 Indeo (no source, Intel IP → use NUT → ffmpeg).

### Recommendation

Now that the native module path is proven to work for Cinepak via `Dec24`, lead
with **native MovieFS modules** for the codecs that ship an r3-free variant
(602/608/615/626 via Dec24; 605 via Dec16; the Dec8 palette family) — it is
bit-exact to Acorn, needs no FFmpeg, and the shared work is small (strip the
16-byte wrapper, select the right variant, add an RGB888/pal8 unpack path). Use
**NUT → FFmpeg** for the screen-painter-only codecs that would otherwise need the
Colour subsystem — chiefly 628/629 Indeo (also no source / Intel IP) — and as a
fallback. Colour-subsystem emulation is, pleasingly, **not on the critical path**.

## Open questions for implementation

- Confirm the MovieFS Replay encapsulation stores the raw codec frame verbatim
  (one Replay chunk = one or more codec frames) and where the palette / FLIC
  header / image-description metadata lives, so extradata can be reconstructed.
  Need a real sample (e.g. RISC DISC 2's `IRONMAN_1/ARMOVIE`, a type-602 movie).
- Decide NUT-only vs. NUT + a minimal AVI/MOV writer for the depth-parameterised
  tranche.
