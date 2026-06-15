# The ARMovie / AE7 container

Authoritative specification of the ARMovie container (RISC OS filetype `&AE7`)
that wraps Replay video and sound. Assumes the conventions in
[methodology.md](methodology.md). Unlike the bitstreams, the container header is
**plain text**; its byte offsets and the catalogue are where the precision
lives.

## 1. File shape

```
+0                      21-line text header (newline-terminated)
...                     payload: chunks of video + sound, in file order
catalogue offset        text catalogue: one line per chunk
sprite offset           RISC OS sprite (poster), `size of sprite` bytes
offset to keys          optional key-frame index table (-1 if absent)
```

The header is at the very start. The catalogue, sprite and key-frame table are
located by byte offsets given **in** the header, so they may appear anywhere in
the file; in practice writers place the payload immediately after the header and
the catalogue/sprite/keys after the payload.

## 2. Text header (20 or 21 lines)

Normally 21 newline-terminated (`\n`; a trailing `\r` is tolerated) lines. Line 1
is the literal magic; the rest are a value optionally followed by descriptive
text after the number — parsers read the **leading token** and ignore the
remainder, so `14 number of chunks` and `14` are equivalent. Trailing spaces are
stripped.

**Line 21 (`offset to keys`) is optional.** Some 1992 sound-only movies
(Head-Line "Welcome to RISC OS 3!" sessions, digitised by Acorn) write only 20
header lines and follow line 20 directly with the poster sprite. A reader must
treat an absent line 21 as `-1` (no keys) rather than reading the sprite bytes as
an over-long line. The catalogue and sprite are located by their absolute
offsets (lines 18–19), so omitting line 21 does not shift them.

A **blank numeric line counts as zero.** Some writers (e.g. `!Morpheus`) leave
the sound fields (lines 11–13) empty when a movie is silent rather than writing
`0`; a reader must treat an empty/whitespace-only numeric line as `0`, not reject
it. See §2.3.

| Line | Field | Type | Notes |
| ---:| --- | --- | --- |
| 1 | `ARMovie` | literal | magic; exact match required |
| 2 | Title | text | may be empty |
| 3 | Copyright | text | may be empty |
| 4 | Author | text | may be empty |
| 5 | Video format | unsigned | compression type (7, 17, 19, 20, 23, …; 0 = sound-only) |
| 6 | Width | unsigned | pixels; 0 only when video format is 0 |
| 7 | Height | unsigned | pixels |
| 8 | Pixel depth | unsigned | bits per pixel (e.g. 16 for 6Y5UV) |
| 9 | Frame rate | real | frames per second; must be finite and > 0 |
| 10 | Sound format | unsigned | sound codec id (0 = no sound) |
| 11 | Sound rate | unsigned | sample rate in Hz — **or** a sample *period* in microseconds when labelled `µs samples`; see §2.3 |
| 12 | Sound channels | unsigned | channel count (blank ⇒ 0) |
| 13 | Sound precision | unsigned | bits per sample (blank ⇒ 0); the *text* also selects the format-1 decoder (see [armovie-sound.md](armovie-sound.md)) |
| 14 | Frames per chunk | real | nominal; a fractional value (e.g. 12.5) is legal |
| 15 | **Number of chunks** | unsigned | the **highest zero-based chunk index**, *not* the count — see §2.1 |
| 16 | Even chunk size | unsigned | declared byte size of even-numbered chunks |
| 17 | Odd chunk size | unsigned | declared byte size of odd-numbered chunks |
| 18 | Catalogue offset | unsigned | byte offset of the catalogue table |
| 19 | Sprite offset | unsigned | byte offset of the poster sprite |
| 20 | Sprite size | unsigned | poster sprite length in bytes (0 if none) |
| 21 | Offset to keys | signed | byte offset of the key-frame table, or **−1** for none; **optional** — may be omitted entirely (treat as −1), see above |

Writers emit human labels after each number, e.g. `19 video format`, `160
pixels`, `12.5 frames per second`, `14 number of chunks`, `-1 (no keys)`.

### 2.1 "Number of chunks" is the last index

Line 15 holds the **highest zero-based chunk number**, so the catalogue contains
`line15 + 1` entries. A value of 14 means 15 chunks, numbered 0–14. Tools should
report both the raw field and the derived entry count to avoid the off-by-one.

### 2.2 Even/odd chunk sizes

Lines 16–17 declare the nominal byte size of even- and odd-indexed chunks. They
support seeking and buffering estimates; the authoritative per-chunk sizes are
the catalogue's, which a reader must use for extraction.

### 2.3 Sound-field variants (blank, and the µs sample period)

Real movies vary how the sound fields (lines 11–13) are written, and a reader
must accept all of these:

- **Blank ⇒ zero.** A silent movie may leave any of lines 11–13 empty instead of
  writing `0` (`!Morpheus` does this for the bits-per-sample line). Treat an
  empty/whitespace-only numeric line as `0`.
- **Sound rate as a microsecond period.** Line 11 is normally a frequency in Hz,
  but some writers give the sample *period* in microseconds, labelled
  `µs samples` (the `µ` is Latin-1 `0xB5`, occasionally the ASCII `us`). For
  example the *Dictionary of the Living World* movies write `72 µs samples`,
  i.e. a 72 µs period = **1 000 000 / 72 ≈ 13 889 Hz** (confirmed by the data:
  ~27 777 mono 8-bit samples per 2 s chunk). A reader keys on the unit text and
  converts to Hz; otherwise a 13.9 kHz track is mistaken for a 72 Hz one and the
  audio plays ~193× too slow.

## 3. Chunk catalogue

At `catalogue offset`, one newline-terminated line per chunk, in chunk order:

```
file_offset,video_bytes[;sound_track_0_bytes[;sound_track_1_bytes...]]
```

- `file_offset` — byte offset of the chunk in the file.
- `video_bytes` — length of the chunk's video region.
- zero or more `;`-separated sound-track byte counts. A reader sums them for the
  chunk's total sound bytes and counts them for the track count.

A well-formed writer emits a sound field even when silent — `;0`, so a typical
line is `405504,147690;27777` (one track) or `0,0;0` (silent). But some real
movies (the *Dictionary of the Living World* series) write a **bare trailing
`;` with no count** for a silent chunk, e.g. `436224,101160;`. A reader must
accept an empty field after a `;` as *no sound track* (zero bytes), not a parse
error. (An empty field between two counts, `…;;5`, is likewise an absent track.)

The shortest legal line is `0,0;0` (6 bytes including the newline). Each chunk's
region `[file_offset, file_offset + video_bytes + Σ sound_bytes)` must lie within
the file. Writers round `file_offset` up to a sector/alignment boundary and pad
the video region to an even length, so a chunk's region may be slightly larger
than its frames strictly need.

## 4. Chunk payload layout

Within a chunk the **video region precedes the sound region(s)**:

```
[ video_bytes of video ][ sound track 0 ][ sound track 1 ]...
```

### 4.1 Video region — not length-prefixed frames

The video region is **not** a table of length-prefixed frames. The player calls
the codec `frames per chunk` times; each call decodes one frame and returns a
pointer to where the next frame begins. Frame boundaries therefore exist only
implicitly in the bitstream. Extracting individual frames requires either parsing
the bitstream grammar (per the relevant codec spec) or running the decoder; you
**cannot** divide `video_bytes` by the frame count. The region may end with one
or more padding bytes after the last frame (e.g. a chunk declaring N video bytes
whose frames consume N−1, leaving a single pad byte).

Inter frames within and across chunks reference the previous reconstructed
frame, so a chunk is not independently decodable unless its first frame is a key
frame; the optional key-frame table (line 21) is what makes seeking possible.

### 4.2 Sound region

Each sound track's bytes follow the video, encoded per the header's sound format
/ rate / channels / precision. For the ADPCM formats each chunk's region begins
with a small per-channel state header (the running coder state at that chunk's
first sample) so a chunk's audio can be decoded after a seek; the PCM formats
store samples directly. The sound formats are specified separately (planned).

## 5. Poster sprite and key-frame table

- **Poster sprite** (lines 19–20): a standard RISC OS sprite (poster) shown
  before playback. **A muxer must embed a real sprite and must not set
  `size of sprite` to 0** — see §6.4: `!ARPlayer` crashes on a zero-length
  sprite. The sprite is a complete RISC OS spritefile; the Replay poster
  convention is 16bpp 1:5:5:5 (red in the low bits) with a new-format
  square-pixel mode word `(5<<27)|(90<<14)|(90<<1)|1` (old numbered modes such as
  28 are 8bpp and render the data as garbage).
- **Key-frame table** (line 21): a byte offset to a table indexing the frames
  that are independently decodable (enabling seeking), or −1 when the movie has
  none. The line itself is optional (see §2) — when absent, read it as −1. Its
  internal layout is not specified here (see Appendix A).

## 6. Player constraints a conforming muxer must satisfy

The container format is defined as much by what the shipped player accepts as by
the header grammar. The ARMovie player engine (version 0.73, 27 Aug 2002; the
BASIC playback loop [`bas/Player,ffb`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/SystemRes/ARMovie/bas/Player%2Cffb))
and its [`!ARPlayer`](https://github.com/barryc-ro/RiscOS_2003/tree/master/RiscOS/Sources/Apps/ARPlayer)
front-end have hard assumptions and outright bugs that are **not** recoverable
errors — a malformed-but-legal file crashes or corrupts playback rather than
being rejected. A muxer must produce files that avoid them.
[`../player-bugs.md`](../player-bugs.md) gives the BASIC line numbers and the
exact failure mechanism for each (and methodology.md gives the source-tree link
convention).

### 6.1 Every chunk must hold exactly `frames per chunk` frames

The player reads line 14 (`frames per chunk`) once into a global and decodes
exactly that many frames from **every** chunk; it never consults the catalogue's
per-chunk frame counts during decode (`bas/Player` reloads its per-chunk
countdown with the global unconditionally). An under-filled chunk is decoded too
many times, reading past its video into the sound bytes and the next chunk, which
also poisons the inter-frame predictor for what follows. **Pad every chunk to a
full `frames per chunk`** (e.g. with repeat-last-frame frames), including the
last.

### 6.2 A one-chunk movie with audio corrupts itself

The player double-buffers chunks and **aliases the two buffers when there is only
one chunk** (`IFmaxfile=0 op1%=op0%`). With sound, the streaming look-ahead then
prefetches the non-existent next chunk into the buffer still being played,
corrupting video a few frames in and losing the audio. **A movie with audio must
have at least two chunks.** (A silent single-chunk movie is fine.)

### 6.3 `frames per chunk` must be at least 3

The player derives its decompress-ahead buffer count from `frames per chunk` and
requires it to stay strictly below that value; at 1 or 2 the clamps collapse the
look-ahead margin. **`frames per chunk` must be ≥ 3.** (Combined with §6.2, a
very short movie with audio must still be split into two chunks of ≥ 3 frames
each — pad as needed.)

### 6.4 `!ARPlayer` crashes on a missing poster sprite

`!ARPlayer`'s `display__loadsprite`
([`Apps/ARPlayer/c/display:110`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Apps/ARPlayer/c/display#L110))
loads the poster with `spr_ReadSprite(area, f, sprite_offset + 12,
sprite_size − 12)` and never checks that `size of sprite` is non-zero (the `+12`
/ `−12` skips the spritefile's 12-byte area header). `spr_ReadSprite`
([`Lib/ARLib/Spr/c/spr:493`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Lib/ARLib/Spr/c/spr#L493))
guards only on `length != 0`, **not** `length > 0` (line 496), so a zero-size
sprite passes `length = −12` to `extendby` and `file_read` (lines 498/504) — a
negative-length read that faults instead of returning an error. Because it faults
rather than failing cleanly, the "fall back to the default sprite" path
([`display:120`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Apps/ARPlayer/c/display#L120))
is never reached and the front-end crashes. (The command-line player only
reads-and-discards the sprite, so it is unaffected — which is why this is easy to
miss.) **Always embed a complete poster sprite** with a non-zero `size of sprite`
(§5); offer an explicit "no poster" mode only if you accept that such files crash
`!ARPlayer`.

### 6.5 Sound-only movies and the sound field

- A **sound-only** movie (video format 0, no frames) must report **0×0**
  dimensions on lines 6–7.
- A conforming muxer should write a sound field — `;0` when silent — for
  readability and round-tripping. A *reader*, however, must tolerate the bare
  trailing `;` that some real movies use for a silent chunk (§3).

## Appendix A. Provenance and corrections

Sources: the ARMovie container as the shipped Acorn player reads it — engine
`RiscOS/Sources/SystemRes/ARMovie` (`bas/Player`, v0.73, 27 Aug 2002) and the
`!ARPlayer` front-end — together with real movies, most directly `LionFish19,ae7`
(SHA-256 `e4a6539b19a105e80e3171a4753870b184edafded0ee874bf2f470231b661684`):
type 19, 160×128 16-bit 6Y5UV, 12.5 fps, 25 frames/chunk, last chunk index 14
(so 15 chunks, 375 frames), no key table, one 44,100-byte sound region per chunk.
Its chunk 0 starts at offset 83,968 and declares 181,886 video bytes; running the
compiled Decomp19 25 times consumes 181,885 and leaves one pad byte, and all 25
frames match byte-for-byte. The header field meanings and the §6 constraints are
read from that player source; this repository's `replay_ae7.c` /
`replay_ae7_write.c` embody them as a cross-check and are not the authority.

Corrections and clarifications:

- **"Number of chunks" is the last index, not the count** (§2.1). This off-by-one
  is the most common misreading of the header and is easy to get wrong because
  the label literally says "number of chunks".
- **The video region is codec-driven, not self-delimiting** (§4.1). Nothing in
  the catalogue or header marks frame boundaries; this was established by
  instrumenting the compiled decoder under emulation to record the source
  pointer it returns per frame (`tools/decomp19_unicorn.py --frames N`), rather
  than guessing from catalogue averages or byte patterns. The trailing-pad-byte
  behaviour was observed the same way.
- **Sound fields vary; a reader must be liberal (§2.3, §3).** The sound rate may
  be a microsecond sample period (`72 µs samples` ⇒ ~13 889 Hz) rather than Hz; a
  silent movie may leave the numeric sound fields (lines 11–13) blank rather than
  writing `0`; and a silent chunk's catalogue line may end with a bare `;` with
  no count. These were all found in real movies (the *Dictionary of the Living
  World* / `!Morpheus` collections) and a reader that assumes Hz, requires the
  fields, or insists on `;0` mis-decodes or rejects them. A *writer* should still
  emit Hz, explicit `0`s and `;0` for clarity.
- **Numeric header fields carry trailing prose.** Real files write
  `19 video format`, `-1 (no keys)`, etc. A parser must read only the leading
  number. Conversely a writer should emit the labels for human/tool readability.
- **Key-frame table layout is unspecified here.** The reference samples
  (LionFish19 and the generated movies) have no key table, so its internal format
  has not been verified against a real example. The header field (offset / −1) is
  specified; the table body is a known gap, to be filled from a movie that uses
  one.
- **Even/odd chunk sizes are advisory.** Lines 16–17 are not authoritative for
  extraction; the catalogue is. They are retained because writers populate them
  and some players use them for buffering.
