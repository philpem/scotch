# Replay AE7 Container Writer (Join) Notes

Authoritative source for the writer: Acorn's own `Join` tool
(`RiscOS_2003/RiscOS/Sources/SystemRes/ARMovie/Tools/Join/c/join`) and its
documentation (`ARMovie_2003/Resources/Documents/Join`). This note records the
exact layout so the portable writer can reproduce it.

## What Join does

Join assembles a finished ARMovie/AE7 file from separate components in a source
directory:

- `Header` — a short text template (14 lines, see below).
- a sprite (poster image).
- `Images<dir>.<nn>` — compressed video, **one file per chunk** (not per frame).
  Each file already contains all the frames of that chunk concatenated, in the
  order the codec emitted them. Files are grouped 77 per directory
  (`Images0.00`..`Images0.76`, then `Images1.00`...).
- `Keys<dir>.<nn>` — optional per-chunk key-frame state, same naming.
- sound: `Sound`/`Sound2`... (8-bit), `Samples`... (16-bit), or `Adpcm`...
  (4-bit), or sound lifted from other ARMovie files via `-armovie`.

Join computes the catalogue, all file offsets, the even/odd chunk sizes, and the
key-frame offset, then writes the 21-line ARMovie header followed by the data.

Our compressor emits one raw payload per frame. Our writer therefore also does
the **chunking** step (group frames into chunks) that CompLib's
`PROCsavechunk` did when it wrote per-chunk `Images` files.

## File layout (video movie)

```
+-----------------------------+  offset 0
| 21-line text header         |
+-----------------------------+  = "offset to sprite"  (int_b)
| sprite (poster), optional   |
+-----------------------------+  = "offset to keys"    (int_d)
| keys area: numChunks*keySz  |  (absent if no keys)
+-----------------------------+  = "catalogue offset"  (int_a)
| text catalogue              |
+--- pad to sector boundary --+
| chunk 0  [video][sound...]  |  first data offset = align_up(cat end)
+--- pad to sector boundary --+
| chunk 1  [video][sound...]  |
| ...                         |
+-----------------------------+
```

- **Sector alignment.** Every chunk's file offset is rounded up to a sector
  boundary with `(x + secsize) & ~secsize`. `secsize` is the `-size N` option,
  **default 2047**, i.e. a 2048-byte (CD-ROM sector) alignment. Inter-chunk gaps
  are zero padding. Verified against `LionFish19,ae7`: every chunk offset is a
  multiple of 2048.
- **Even video length.** Each chunk's video region is padded to an even
  (halfword) length with one `0` byte if odd. Verified: every `video_bytes` in
  the LionFish catalogue is even; chunk 0 decodes 181885 bytes and is stored as
  181886.

## 21-line header (what the writer emits)

Field meanings and the player-visible numeric prefix (text after the number is a
human annotation the parser ignores):

| # | Example                       | Meaning |
|---|-------------------------------|---------|
| 1 | `ARMovie`                     | magic |
| 2 | `Lion Fish in Red Sea`        | title |
| 3 | `© 1991 ...`                  | copyright |
| 4 | `Digitised by Uniqueway`      | author |
| 5 | `19 video format`             | video codec (0 = no video) |
| 6 | `160 pixels`                  | width |
| 7 | `128 pixels`                  | height |
| 8 | `16 bits per pixel [6Y5UV]`   | pixel depth |
| 9 | `12.5 frames per second`      | fps (double) |
| 10| `1 sound format`              | sound codec (0 = no sound) |
| 11| `11025 Hz samples`            | sound rate |
| 12| `2 channels`                  | channels ("reversed" = swapped L/R) |
| 13| `8 bit VIDC1 µ-law`           | sound precision bits + companding label |
| 14| `25 frames per chunk`         | nominal frames per chunk |
| 15| `14 number of chunks`         | **last** zero-based chunk index |
| 16| `284501 even chunk size`      | see below |
| 17| `267591 odd chunk size`       | see below |
| 18| `82578 catalogue offset`      | int_a |
| 19| `602 offset to sprite`        | int_b |
| 20| `81976 size of sprite`        | sprite bytes (0 allowed) |
| 21| `-1 (no keys)`                | key-frame offset, or `-1` |

The three offset fields are forward references, so the writer renders the header,
measures it, recomputes the offsets, and re-renders until the byte length is
stable (Join uses `XXXXXXX` placeholders then patches).

## Even / odd chunk size

These are buffer-allocation hints. Replay double-buffers: even-indexed chunks
load into one buffer, odd into another, so each buffer must hold the largest
chunk of its parity. From `Join` (`c/join` ~line 669):

```
evenmaxsize = max_even_video + sndsize + 1;   (+4 mono / +8 stereo if adpcm)
oddmaxsize  = max_odd_video  + sndsize + 1;
```

i.e. **max (video + sound) over that parity, plus one guard byte.** Verified:
LionFish max even video 240400 + 44100 + 1 = 284501; max odd 223490 + 44100 + 1
= 267591. The portable writer computes `max(video_i + sound_i) + 1` per parity,
which is identical for constant sound and more robust for variable-length
chunks.

## Fractional frame rates and even/odd

A chunk holds an integer number of frames, but a "1 second" chunk at 12.5 fps
cannot. The clean rule (and what makes even/odd sizes legitimately differ by
frame count, not just compression) is to let the per-chunk frame count alternate:

```
frames_in_chunk(i) = floor((i+1) * F) - floor(i * F)      F = fps * chunk_seconds
```

For F = 12.5 this yields 12, 13, 12, 13, … averaging exactly 12.5. The header's
`frames per chunk` is then the nominal `round(F)`. Because type 19 is temporal,
the player decodes a chunk by consuming its `video_bytes` frame by frame, so a
varying frame count per chunk is fine. The audio per chunk is sliced by the same
time boundaries so A/V stays in sync. (NTSC-style 30000/1001 rates work the same
way — F is just non-integer and the floor-difference distributes frames.)

LionFish itself uses a constant 25 frames/chunk (fps 12.5 → 2.0 s exactly), so
its even/odd sizes differ purely from compression variance; the alternating-count
behaviour only appears when `chunk_seconds * fps` is non-integer.

## Sound interleaving and formats

Within a chunk the layout is `[video][track0][track1]...`. The catalogue line is
`offset,video_bytes;track0_bytes[;track1_bytes...]`. Per-chunk sound bytes track
the chunk's time span: `bytes = samples_in_span * channels * bytes_per_sample`.

Sound formats (header line 10). Per `AE7doc` the meaning of this field is
defined by the video codec, but the conventional numbers are:

- `0` — no sound.
- `1` — built-in 8-bit VIDC "exponential" companding (VIDC1 µ-law; `Sound`
  files). One byte per sample per channel. **Default.**
- `2` — the *extensible named-decompressor* format. The header line is written
  `2 <name>` and the player loads that sound decompressor (`AE7doc`: "If the
  sound format is 2, the name of the sound decompressor file follows after a
  single space: `2 soundxxx`"). **ADPCM, G721, G723-1/24/40, GSM, MPEG-I and
  MPEG-II are all format 2 with a name** — e.g. `2 adpcm`, `2 GSM` — living in
  `ARMovie_2003/Sound16/<name>`. AE7doc directs all new sound codecs to format
  2. AREncode exposes these plus 8-bit VIDC and an uncompressed option.

`CompSound` defines the format-2 codec interface: a `CInfo` "input frame sample
size" (0 = arbitrary samples per chunk, e.g. GSM uses 160) and `Encode`/`Decode`
entry points taking signed 16-bit linear input. So **framed/variable-rate
format-2 tracks encode whole sample-frames per chunk** and their byte sizes are
not `samples * bytes_per_sample`; those need chunk boundaries supplied
explicitly. The writer's automatic time-slicing is correct for format 1 and
uncompressed linear PCM only.

Join itself only *builds* with no-sound / format 1 / format 2 (and can compress
16-bit down to `2 adpcm`); the broader codec set is produced by the separate
`Sound16` encoders. So the container writer is audio codec-agnostic: it
concatenates already-encoded track bytes and records their sizes. A PCM→VIDC-µ-law
encoder (and a framed-codec chunk-slicing path) is the remaining audio work for
ingesting WAV/raw PCM directly.

## AREncode / Join options mapped to the writer

- **Index (numeric)** — `-index N`. Selects a numbered component set
  (`NHeader`, `NImages0`, `NKeys0`). Pure file-naming for compressing/joining a
  movie in separately-numbered segments. Not needed for single-pass output;
  expose as an optional output-name prefix only if we add segmented batches.
- **Join keys (on/off)** — inverse of `-nokeys`. When on, Join copies the
  per-chunk `Keys` blobs into the movie and sets `offset to keys`; when off it
  writes `-1 (no keys)`. Keys give the player a chunk-boundary restart state for
  random access into a temporal stream. For type 19 a key is the squashed
  previous reconstructed frame (w*h*2 bytes). The writer plumbs a `write_keys`
  flag and a per-chunk key-blob input; default off (`-1`), matching the verified
  LionFish sample.
- **-size N** — sector alignment mask (default 2047 → 2048). Smaller = smaller
  file, larger = better streaming on high-latency media.

AREncode's three rate-control **modes** drive the *compressor*, not the writer,
and map onto our existing `replay-encode` controls:

- **Quality factor** → `--loss-level` (fixed quality, variable size).
- **Frame size (bytes)** → `--target-bytes` (fixed size, variable quality);
  "Faster Matching" ≈ a reduced motion search, "Limit to ARM2" ≈ clamp decode
  cost for slow CPUs (CompLib `-arm2`).
- **Device bandwidth** (latency seconds default 0.4, data rate kB/s default 150,
  "Assume Double Buffers") → a not-yet-implemented driver that converts a target
  kB/s + fps into a per-frame/per-chunk byte budget and feeds `--target-bytes`.
  CompLib computes this from `datarate`, `latency`, and `double`.

## Player compatibility constraints (from the v0.53 Player and !ARPlayer source)

Found by detokenising the running player (`riscos-basic-detokenise`, built with
gcc) and reading `Apps/ARPlayer` + `Lib/ARLib`. All four are enforced by the
portable writer:

- **An audio movie must have at least two chunks.** The player double-buffers
  and aliases its two chunk buffers (`op1%=op0%`) for a one-chunk movie, so the
  look-ahead prefetch of the (non-existent) next chunk overwrites the chunk being
  played: video corrupts a few frames in and sound is lost. Multi-chunk movies
  (like LionFish's 15) are fine. The writer forces ≥2 chunks whenever there is
  sound.
- **`frames per chunk` must size the largest chunk's sound.** The player's single
  sound buffer is `dblsnd% = (fpf/fps * rate * channels * 1.01 * bits)/8 + ~19`,
  using the header `fpf`. With fractional chunking (12/13 for 12.5) the average
  would undersize the buffer for the longer chunks, so the writer reports the
  maximum chunk's frame count as `fpf` when there is sound.
- **`frames per chunk` must be ≥ 3.** The player computes `C% = fpf-1` and raises
  error E05 ("not enough memory") if `C% < 2`.
- **A sound-only movie (video format 0) must report 0×0 dimensions.** Non-zero
  dimensions with no video decompressor crash the player.
- **Multiplexing matches Join exactly.** Per chunk `[even-padded video][sound]`;
  the player reads sound from `op0 + video_bytes` for `sound_bytes` (catalogue
  field), the same place Join writes it (`myj = op + ImageFileSize`).
- The catalogue always carries a sound field; a silent movie uses `;0`.

## Sound: VIDC 8-bit "exponential" (E format)

`replay_sound` encodes signed-16 PCM (from ffmpeg `-f s16le`) to format-1
sub-formats; the bits-per-sample label selects the player's decoder (per
`ToUseSound`: ADPCM/LIN/UNSIGN → SoundA/S/U, else SoundE). VIDC E8 is the
nearest-match inverse of Acorn's exact `ELogToLinTable`
(`Lib/.../Audio/SoundFile/s/e8to16`), ~37 dB round-trip; 8/16-bit signed linear
are a byte copy / shift.

## The "helpful sprite" (poster) — required by !ARPlayer

`Apps/ARPlayer/c/display` loads the poster with
`spr_ReadSprite(f, sprite_offset+12, sprite_size-12)` and (ARLib `Spr/c/spr`)
`file_read`s that length whenever it is non-zero. A movie with **no** sprite
(`sprite_size 0 → length -12`) therefore **crashes !ARPlayer** — the command-line
Player only ever reads-and-discards the sprite, so it never cared. The Dummy
movie survives only because ARPlayer loads it with `filename == NULL`.

The poster is a complete standard RISC OS **spritefile**: a 12-byte area header
(`num_sprites=1`, `first_sprite_offset=16`, `free_offset=filesize+4`) then one
sprite (control block + image); ARPlayer reads from `sprite_offset+12` for
`sprite_size-12` bytes. The writer's poster is **16bpp (1:5:5:5, red in the low
bits = ffmpeg `bgr555le`)** using a *new-format square-pixel* mode word
`(5<<27)|(90<<14)|(90<<1)|1` = `0x281680B5`. Old numbered modes such as 28 are
8bpp (256-colour) and render 16bpp data as garbage; square pixels mean the poster
is stored at the movie's true dimensions (no 2:1 height doubling that an 8bpp
mode-28 poster like LionFish's needs).

`replay-join` embeds the built-in **Replay-logo Default sprite**
(`!ARMovie.Default`, 160×128 8bpp mode 12) when no `--poster` is given, so no
movie can accidentally crash !ARPlayer; `--poster FILE.bgr555` overrides it and
`--no-poster` writes `sprite_offset/size 0` (command-line player only).

## One-shot driver

`tools/replay-make INPUT OUTPUT,ae7 [--width N --fps F --loss-level N ...]` wraps
ffmpeg + `replay-encode` + `replay-join`: it auto-computes an aspect-correct,
÷4 height, encodes video, extracts mono VIDC-E8 audio, builds a poster from the
first frame (or `--poster IMAGE`), and joins, cleaning up the intermediate files.

## Key frames (chunk seeking)

The "key frame offset" field points to a block per chunk except the first
(chunk_count-1 blocks of width*height*2 bytes). The v0.53 player, to start at
chunk `fchunk` (fchunk != 0), reads `ipsz% = width*height*2` bytes from
`KF + (fchunk-1)*ipsz%` and runs `.keyer` to expand them into the previous-frame
buffer, so key block j is the start state for chunk j+1: the reconstruction at
the *end* of chunk j. Chunk 0 starts unaided (its first frame is intra).

Each block is the reconstructed frame packed as 6Y5UV halfwords -- one
little-endian halfword per pixel, `Y[0:5] | U[6:10] | V[11:15]` (the same layout
as type 2 `type19-fields`). `replay-encode --keys-prefix` writes one such block
per frame; `replay_ae7_write` is given the per-frame blobs and selects the
end-of-chunk frames itself; `replay-join --keys-prefix` and `replay-make --keys`
drive it. Verified: for 60 frames at 25 frames/chunk the two emitted keys are
byte-identical to the reconstructions of frames 24 and 49.

## ADPCM (mono)

Acorn's "adpcm" sound is standard IMA/DVI ADPCM. Because it is stateful and the
player must be able to start each chunk's sound independently, each chunk's
sound region is a 4-byte state header -- valprev (little-endian 16-bit), then the
step index, then a pad byte -- followed by the chunk's samples as 4-bit codes
(two per byte, first sample low nibble). The running encoder state carries across
chunks; the header captures it at each chunk's first sample. This is the +4 (mono)
that Join adds to the even/odd chunk sizes.

The writer takes raw s16 PCM (track.encode_adpcm) and encodes per chunk itself,
so sound_bytes = 4 + ceil(chunk_samples/2). Two header flavours select the same
bytes: format 1 with precision 4 and an "ADPCM" label picks the built-in SoundA4
decoder, while format 2 "adpcm" uses the named sound decompressor.
Stereo carries an 8-byte header (left then right state) and one byte per stereo
frame -- the left code in the low nibble, the right in the high -- with a
separate running state per channel, exactly as Join's `Decodex2` expects.

`--sound-encode adpcm` (and `adpcm2`) select the format-2 named decompressor
`2 adpcm`, which is the path most systems ship (SoundA4 is often absent).
`--sound-encode adpcm-sounda4` selects the built-in format-1 SoundA4.
