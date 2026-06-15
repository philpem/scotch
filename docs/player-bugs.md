# Acorn Replay player bugs and constraints

The shipped Acorn Replay player has several bugs and hard assumptions that a
movie file must work around — they are not recoverable errors, so the *writer*
and *encoder* have to produce files that avoid them. This documents each one,
where it lives in the player code, and how the tooling here satisfies it.

## Versions examined

- **Player engine: ARMovie 0.73, dated 27 Aug 2002** — RISC OS source component
  `RiscOS/Sources/SystemRes/ARMovie` (`VersionNum`: `Module_MajorVersion 0.73`).
  The relevant file is `bas/Player` (`Player,ffb`), a BBC BASIC program that
  assembles its inner video loop with `[OPT`. In the tree examined it was last
  changed in git commit `00a2b46` (2001-07-14); the matching assembled binary
  ships as `Player073` (e.g. `Replay/OldARM/!ARMovie/Player073,ffb`). Some
  constraints were originally confirmed against the earlier **v0.53** player and
  emulator testing; the behaviour is long-standing across player revisions.
- **GUI front-end: `!ARPlayer`** (bug 4 below) — a separate application from the
  player engine.

Line numbers below are BASIC line numbers in `bas/Player`. To reproduce,
detokenise with
`riscos-basic-detokenise/posix/bastotxt -i Player,ffb -o player.txt`.

---

## 1. Every chunk must hold exactly "frames per chunk" frames

The player reads the header's **"frames per chunk" (`fpf`) once as a single
global** and decodes exactly that many frames from every chunk; it never
consults the catalogue's per-chunk counts during decode. An under-filled chunk
is therefore decoded `fpf` times, and the surplus decodes read past the chunk's
video into the interleaved sound bytes (and the next chunk) — a corrupt frame
that, without key frames, also poisons the predictor for the following chunk.

`fpf` is read from the header:

```
9080  fpf=VALGET$#file%        REM the "frames per chunk" header line
```

The decompressor keeps `run` = frames left in the current chunk, initialised to
`fpf`, decremented per decoded frame; while non-zero it stays in the same chunk:

```
40310 .run  DCD fpf
41070  LDR r0,run
41080  SUBS r0,r0,#1
41090  BNE notbend            REM frames left -> decode another from this chunk
```

When `run` hits zero it switches to the next chunk's buffer and **reloads `run`
with `fpf` unconditionally**, regardless of that chunk's real frame count:

```
41330 .halfbend
41340  STR r2,addb            REM repoint the decompressor at the next chunk
41350  MOV r0,#fpf            REM reload the per-chunk countdown
41360 .notbend
41370  STR r0,run
```

Frame numbering assumes it too (`15460 !rn%=fchunk%*fpf`), as does end-of-movie
timing on the streaming path: it uses `(maxfile+1)*fpf` frames; only the in-core
"quick" path derives the last chunk's real length from the catalogue
(`cat%!(maxfile*12) DIV fsz%`), and even then the decompressor loop above is
unchanged, so a short *non-final* chunk still corrupts playback on every path:

```
14810 endchunk=maxfile+1:endchunkpart=0:IFsourceisquick% IFfetcher$="" endchunk=maxfile:endchunkpart=cat%!(maxfile*12)DIVfsz%
14830 endtime=(endchunk*fpf+endchunkpart)*100000/fps: ...
```

The writer emits `fpf = max chunk frame count` (`src/replay_ae7_write.c`,
`frames_per_chunk_nominal`), so uneven chunks (e.g. 12 and 13) write `fpf=13` and
the 12-frame chunk under-fills.

**Avoided by:** padding the final chunk to a full `fpf` with repeat-last-frame
frames (`mb_repeat_payload`; `replay-encode --pad-to-multiple`, on by default in
`replay-make`), and even-chunk planning for short clips (bug 2).

## 2. A one-chunk movie with audio corrupts itself (double-buffer aliasing)

The player double-buffers chunks. When a movie has only one chunk it **aliases
its two buffers** (`op1% = op0%`):

```
12190 IFmaxfile=0 op1%=op0%                 REM maxfile = last chunk index; 0 = one chunk
12200 IFmaxfile=1 AND twobuf% op1%=op0%     REM (also when a 2-chunk movie fits in core)
```

With sound this is fatal when streaming: the look-ahead prefetch of the
(non-existent) next chunk writes into the aliased buffer that is still being
played, corrupting video a few frames in and losing the sound. (When the movie
fits in core — `twobuf%`/`incore%`, lines 12200–12230 — there is no prefetch, so
the aliasing is harmless.)

**Avoided by:** forcing **at least two chunks** whenever a movie has audio
(`src/replay_ae7_write.c`). Note this is what makes a *short* audio movie get
split, which then runs into bug 1 if the split is uneven — see bug 3 and
`pad_sink_for_chunks` in `tools/replay_encode.c`, which splits a sub-chunk audio
movie into two **equal** full chunks.

## 3. Frames-per-chunk must be at least 3

The decompress-ahead buffer count `C%` is derived from `fpf` and must stay
strictly below `fpf` (it needs at least one frame of margin for the
double-buffering to work):

```
10460 C%=fpf/2:IFC%<4 C%=4
10530 IFC%>=fpf C%=fpf-1
10580 IFC%>=fpf C%=fpf-1:IFC%<2 C%=2     REM low-memory path: also floors C% at 2
```

If `fpf` is 1 or 2 the `C% = fpf-1` / `C% >= 2` clamps collapse the margin (with
`fpf=2`, `C%` is forced to 2 = `fpf`), breaking the look-ahead. So **`fpf` must
be >= 3**.

**Avoided by:** the default `frames-per-chunk` is 25; the short-clip even-chunk
planner floors the effective frames-per-chunk at 3 (`pad_sink_for_chunks`).

## 4. `!ARPlayer` crashes on a missing poster sprite

The GUI front-end `!ARPlayer` reads the movie's "helpful sprite" (poster) as
`spr_ReadSprite(sprite_offset + 12, sprite_size - 12)` with no guard. A movie
written with no sprite (size 0) yields a `-12`-byte read and crashes the front
end. The command-line player does not read the sprite and is unaffected.

**Avoided by:** the writer always embeds a complete RISC OS spritefile — a real
poster (`--poster FILE.bgr555`) or the built-in Replay-logo default — and refuses
to write a video movie without one unless `--no-poster` is given
(`tools/replay_join.c`, `tools/default_poster.h`, `src/replay_movie.c`). The
poster is 16bpp (1:5:5:5, red in the low bits) with a new-format square-pixel
mode word `(5<<27)|(90<<14)|(90<<1)|1`; old numbered modes such as 28 are 8bpp
and render the data as garbage.

## 5. Other player-enforced format constraints

Not bugs exactly, but the player rejects or mishandles movies that break these
(learned from the v0.53 player source and emulator testing):

- A **sound-only** movie (video format 0, no frames) must report **0x0**
  dimensions.
- The catalogue must **always carry a sound field** — `;0` when silent — not omit
  it.
- **Every chunk must hold exactly `frames per chunk` frames**, including the last
  one. The player decodes a fixed `frames per chunk` from each chunk and never
  consults a per-chunk frame count, so an under-filled final chunk is decoded
  past its data — playback runs off the end of the real video and shows frozen or
  stale frames. The encoder pads the final chunk up to a whole multiple of
  `frames per chunk` with "repeat last frame" frames (a frozen tail, with silence
  once the audio runs out) rather than ever shipping a short chunk; for the
  direct-to-container path (`replay-encode --output`) this padding is
  **unconditional**, not gated on `--pad-to-multiple`.

Both are enforced by `src/replay_ae7_write.c`. End-to-end verified: a four-chunk
160x96 type-19 movie with 11025 Hz VIDC-E8 audio plays with sound in the RISC OS
Replay player.

## 6. Cosmetic: an RGB movie's colour shows a stray `[` in Movie Info

`!ARPlayer`'s Movie Info box builds its video line as `"%dbpp %s %gHz, %s"` with
the colour from `arvid_colourspace(hdr->flags)`
([`Apps/ARPlayer/c/info`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Apps/ARPlayer/c/info),
the `VIDEO_FIELD` `dbox_setfieldf`). The colourspace flag is parsed in
[`Lib/ARLib/ARLib/c/arhdr`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Lib/ARLib/ARLib/c/arhdr)
(`arline_PixelDepth`) and it only **positively recognises non-RGB** spaces:
`strstr(buffer,"YUV")` → YUV, `"PALETTE"` → Palette, 8bpp → Grey. There is **no
RGB test** — RGB is the implicit default (`colourspace_RGB == 0`).

The upshot, observed under emulation: a movie whose pixel label the player does
not positively recognise falls back to echoing the *raw* `bits per pixel [...]`
label, so an **RGB** movie shows the bracketed label (`...16bpp [RGB 12.5 Hz...`)
while a **YUV** movie — positively matched — shows a clean `YUV`. The stray `[`
is therefore the player echoing our own bracketed colour label for the
unrecognised-RGB case; it is purely cosmetic (the colour-map selection in
`bas/Player` strips the brackets correctly, so both movies still load the right
`MovingLine.ColourMap` and play — see [moving-lines spec
§colour](spec/type1-moving-lines.md) and the `replay-encode --codec 1` muxer).

**Fix (applied):** `replay-encode --codec 1` now writes the RGB pixel-depth line
**label-less** (`16 bits per pixel`, no `[RGB]`). The player has no way to
recognise RGB positively, so RGB is its default — `bas/Player` line 2140 sets
`f$="rgb"` when no bracket and no `YUV` are found, still resolving
`ColourMap.RGB16` — and with no bracket to echo, ARPlayer's Movie Info shows a
clean `RGB`. YUV keeps its label: `bas/Player` line 2120 needs the literal `YUV`
to select `YUV16`, and a label-less YUV header would wrongly fall through to the
`rgb` default. So only YUV is labelled; RGB relies on the documented default.
