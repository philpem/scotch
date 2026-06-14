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

## 2. Text header (21 lines)

Exactly 21 newline-terminated (`\n`; a trailing `\r` is tolerated) lines. Line 1
is the literal magic; the rest are a value optionally followed by descriptive
text after the number — parsers read the **leading token** and ignore the
remainder, so `14 number of chunks` and `14` are equivalent. Trailing spaces are
stripped.

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
| 11 | Sound rate | unsigned | sample rate in Hz |
| 12 | Sound channels | unsigned | channel count |
| 13 | Sound precision | unsigned | bits per sample |
| 14 | Frames per chunk | real | nominal; a fractional value (e.g. 12.5) is legal |
| 15 | **Number of chunks** | unsigned | the **highest zero-based chunk index**, *not* the count — see §2.1 |
| 16 | Even chunk size | unsigned | declared byte size of even-numbered chunks |
| 17 | Odd chunk size | unsigned | declared byte size of odd-numbered chunks |
| 18 | Catalogue offset | unsigned | byte offset of the catalogue table |
| 19 | Sprite offset | unsigned | byte offset of the poster sprite |
| 20 | Sprite size | unsigned | poster sprite length in bytes (0 if none) |
| 21 | Offset to keys | signed | byte offset of the key-frame table, or **−1** for none |

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

## 3. Chunk catalogue

At `catalogue offset`, one newline-terminated line per chunk, in chunk order:

```
file_offset,video_bytes[;sound_track_0_bytes[;sound_track_1_bytes...]]
```

- `file_offset` — byte offset of the chunk in the file.
- `video_bytes` — length of the chunk's video region.
- one or more `;`-separated sound-track byte counts. **At least one is always
  present**; a movie with no sound writes `;0`. A reader sums them for the
  chunk's total sound bytes and counts them for the track count.

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

- **Poster sprite** (lines 19–20): a standard RISC OS sprite shown before
  playback. `size of sprite` is 0 when absent.
- **Key-frame table** (line 21): a byte offset to a table indexing the frames
  that are independently decodable (enabling seeking), or −1 when the movie has
  none. Its internal layout is not specified here (see Appendix A).

## Appendix A. Provenance and corrections

Sources: the ARMovie header as parsed and written by the reference
`replay_ae7.c` / `replay_ae7_write.c`, cross-checked against real movies — most
directly `LionFish19,ae7` (SHA-256
`e4a6539b19a105e80e3171a4753870b184edafded0ee874bf2f470231b661684`): type 19,
160×128 16-bit 6Y5UV, 12.5 fps, 25 frames/chunk, last chunk index 14 (so 15
chunks, 375 frames), no key table, one 44,100-byte sound region per chunk. Its
chunk 0 starts at offset 83,968 and declares 181,886 video bytes; running the
compiled Decomp19 25 times consumes 181,885 and leaves one pad byte, and all 25
frames match the reference decoder byte-for-byte.

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
- **The catalogue always carries a sound field.** Even a silent movie writes
  `;0`; a reader must not treat a missing sound field as valid.
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
