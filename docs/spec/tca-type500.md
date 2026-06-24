# Replay type 500 — Iota "The Complete Animator" (TCA / ACEF)

Scoping for Replay video format **500**, the second codec called out in issue #9
(the "All About Planes" educational disc; sample `test-videos/BUCCAN`). Status:
*Video implemented for all screen modes — `src/replay_tca.c` + `replay-transcode`
case 500. 8-bit (28/21/15/36/40) and 4-bit (27/12/13/39) modes decode; validated
end to end on BUCCAN (the Buccaneer) and against the Iota `.tca` corpus
(dino/bang/timer/…). The Iota audio track and multi-chunk handling remain (logged
as issues).*

## Summary

Type 500 is **Iota Software's "The Complete Animator" (TCA)** film, also called
IotaFilm / ACEF. The issue's hypothesis ("the Replay encapsulation is just a TCA
file in a trenchcoat") is **confirmed**: a type-500 Replay movie is a normal
ARMovie/AE7 container whose chunk video payload is a verbatim ACEF film.

`Decomp500` is *not* a CodecIf module: it ships as a `!RunImage` application that
`*RMEnsure`s Iota's **EuclidX** relocatable module (the real LZW/sprite decoder,
SWI-driven) plus **IotaSound** for audio, and its `-explode` writes the film back
out as an IotaFilm file (`&C2A`). So:

- It cannot be driven in the ARMulator/CodecIf harness (EuclidX is a RISC OS
  module needing the SWI/RMA environment, not the three-word CodecIf interface).
- ffmpeg has no TCA decoder.

**Therefore type 500 needs a native C decoder** — a reimplementation of the ACEF
frame decode, guided by the references below.

## Confirmed from the sample (`BUCCAN`)

The AE7 reader already parses it:

```
codec=500  256x192  depth=8  fps=7.1429  chunks=1  frames_per_chunk=17
sound: codec=500 rate=8000 ch=2 bits=8        (Iota sound)
chunk[0] off=50232 video=27044 sound=0
```

The chunk payload at +50232 begins with the **`ACEF`** tag:

```
ACEF  a4690000(=27044 total)  9c690000(=27036)  "Untitled\r\0"  ffff
40000000(=64)  00020000(=512)  80010000(=384)  1c000000(=28)  01000000  08000000(bpp=8)
```

`512×384` is `2×` the pixel size (`256×192`) — the BASIC reads `acef%!20`/`acef%!24`
as window dimensions in OS units (2 OS units per pixel), so pixel size = those / 2.

## IotaFilm layout in the Replay container (confirmed on BUCCAN)

A type-500 movie embeds the **whole IotaFilm** (`&C2A`) contiguously, starting at
the video chunk's `file_offset`. Iota's "Film" format is a chunk file — each chunk
is `[ID:4][size incl. header:u32le][data]`, walked with the documented
`FNfilm_findchunk` (`!start == ID ? start+8 : start += start!4`). BUCCAN's chunks,
contiguous from +50232 to EOF:

| Chunk | @ | size | role |
| ----- | -: | ---: | ---- |
| `ACEF` | 50232 | 27044 | the compressed frames (this is the Replay video chunk) |
| `PALE` | 77276 | 1060 | palette + screen-mode info |
| `FULL` | 78336 | 48 | full-screen mode prefs (Acorn only; ignore) |
| `RATE` | 78384 | 20 | playback rate |
| `DIR1` | 78404 | 160 | sound-effect soundtrack (sequence of play events) |
| `SOUN` | 78564 | 44296 | SoundLib sample library (`&C47`) |
| `FADE` | — | — | fades/pauses between frames — see below; **not used by the Replay path** |

So decode = AE7 reader gives `chunk[0].file_offset`; from there, `FNfilm_findchunk`
for `ACEF` (frames) and `PALE` (palette). (`RATE` is redundant with the Replay
header's fps; `SOUN`/`DIR1` are audio, later.)

### FADE is irrelevant to Replay type 500

The Replay decompressor (`Decomp500/!RunImage`) reads only **`ACEF`, `RATE`,
`DIR1`, `FULL`, `PALE`** via `FNfilm_findchunk` — it never looks for `FADE`. FADE
(inter-frame fades/pauses) is applied by The Complete Animator's own player, not
the Replay path. It is also absent from BUCCAN and its Iota doc is gone (soft-404).
So there is nothing to reverse-engineer and nothing lost: a type-500 transcoder
ignores FADE exactly as the Replay player does.

## ACEF chunk and ACE film header (from Iota `format.txt`, validated on BUCCAN)

`ACEF` = `[id:"ACEF"][offset-to-next:u32le]` then the ACE film data, a 64-byte
header followed by the Euclid data block:

| Off | Field | BUCCAN |
| --: | ----- | ------ |
| 0 | film length (incl header + 0 end word) | 27036 |
| 4 | film name, ≤11 chars ending in a control code (`&0D`) | "Untitled" |
| 16 | offset to film start (Mogul sets 64) | 64 |
| 20 | width in **OS units** (= 2× pixels) | 512 → 256px |
| 24 | height in OS units | 384 → 192px |
| 28 | original RISC OS screen mode | 28 (8bpp) |
| 32 | compression technique (`Euclid` R0): `0=RLE, 1=LZW` | 1 (LZW) |
| 36 | flags: bits0-1 `0=Normal,1=Delta`; bits2-3 loop `0 stop/1 repeat/2 yoyo` | 8 → Normal, yoyo |

### Frame blocks (the Euclid data block)

From `film + offset-to-film-start` (64): a sequence of frame blocks, each
`[len:u32le][compressed screen data][len:u32le]`, advance by the leading `len`;
a `len == 0` word terminates. Confirmed: BUCCAN walks **17 frames** (matching
`frames_per_chunk`) ending exactly at the zero word. Decode each block's data with
the film's technique (RLE/LZW); for `Delta` films XOR the result onto the previous
screen (BUCCAN is `Normal`, i.e. full screens). Euclid_Expand writes raw screen
bytes for the mode (8bpp → `width*height` index bytes; mind RISC OS row word
alignment), **not** a sprite-with-header.

### Decode algorithm (resolved, from Nihav `codecs/euclid.rs`)

Per frame: the packet is `block[4 .. len]` (the data after the leading length
word; the trailing length word is ignored). Decode by technique into a
frame-sized buffer (`frm_size`), then for a **Delta** film XOR it onto the
running frame, else it replaces the running frame. `update = flags & 1`.

- **LZW (technique 1)** — bits are read **LSB-first** (`BitReaderMode::LE`). The
  dictionary **resets every frame**. Skip the first **9 bits**, then read
  `idx_bits`-wide codes starting at **9 bits**: code `256` ends the frame; codes
  `< dict_pos` decode via the `dict_prev`/`dict_sym` backlink chain (emit
  reversed, base symbol ≤ 256); the `== dict_pos` case is the standard KwKwK; new
  entries are added from `dict_pos = 257`; when `dict_pos == dict_lim` the width
  grows (`dict_lim <<= 1; idx_bits += 1`) up to 16 bits. Decode must fill exactly
  `frm_size` bytes.
- **RLE (technique 0)** — byte runs: read `len`; if `0`, read another byte (if
  also `0`, read a 24-bit big-endian length, else `len = (b1<<8)|b2`); the run
  pixel is `read_byte()` when `len` is even, else `0`; `len >>= 1`; fill `len`
  pixels. (No RLE films in the corpus yet — documented, untested.)
- **raw (technique 2)** — copy `frm_size` bytes verbatim.

`frm_size` and the output layout depend on the screen `mode`: **28/21 = 8-bit
direct** (`width*height`, one byte per pixel — copy `width` bytes per row);
**27 = 4-bit full height** (`(width/2)*height`, two pixels per byte); **12/13/39
= 4-bit half-res** (also vertically doubled on output); **15/36/40 = 8-bit
row-doubled**.

### Validation (prototype)

A standalone C prototype of the above (mode 28, LZW + raw, normal + delta)
decodes correctly:

- **katie** (320×256, mode 28, Normal) — a Border Collie and ball;
- **toast** (600×450, mode 28, **Delta**, 60 frames) — frame 30 is a clean
  toaster-element diagram, so the per-frame XOR delta is correct;
- **BUCCAN** (the Replay-embedded sample, decoded from the IotaFilm at
  `chunk[0].file_offset`) — the Buccaneer aircraft.

A 27-film TCA corpus (`test-videos/tca-films/`, fetched from iota.co.uk, not
committed) is the dev/validation set; it covers modes 12/15/27/28, Normal and
Delta, with and without sound — all parse, and every mode-28 film decodes.

## PALE chunk (from Iota PALE page, validated on BUCCAN)

`[id:"PALE"][size:u32le]` then words: `+8` pencil colour, `+12` paper colour,
`+16` ModeFlags (bit 7 = Acorn "true" 256-colour palette), `+20` Log2BPP
(`3 = 8bpp`), `+24` Log2BPC, `+28` XEigFactor, `+32` YEigFactor, `+36..` the
palette: one **ColourTrans** word per logical colour. On disk each word is
`[index:u8][R:u8][G:u8][B:u8]` (LE) — verified on BUCCAN: idx0=`00 00 00 00`
(black), idx4=`04 44 00 00` (R=0x44). So `R=byte1, G=byte2, B=byte3`; BUCCAN has
256 entries (size 1060 = 36 + 256×4). Build a 256×3 RGB table → `COL_PAL8`.

## Audio (`SOUN` + `DIR1`) — for later, from Iota `soundlib.txt`

Iota audio is **event-driven**, not a linear PCM track: `DIR1` is a soundtrack of
play events and `SOUN` is the SoundLib sample library they reference (via
`IotaSound_Play`, whose interface mirrors `Euclid_Expand` — one block per frame).

- **`DIR1`** sound blocks (per `format.txt`): `[next→][sound_id/flags][amplitude]
  [pitch][duration]…` (up to 8 voices) `[←previous]`, size in the first/last word.
- **`SOUN`** = a SoundLib file (`&C47`), itself a chunk file with a `NAM1` (sample
  names) and one of `WAV1`/`WAV2`/`WAV3`:
  - per-sample header is 4 words: offset, length, default pitch, format/extra.
  - `WAV1` = 8-bit VIDC-logarithmic samples (length in bytes).
  - `WAV2` = 4-bit IMA-style ADPCM of 16-bit signed (length in samples).
  - `WAV3` = explicit format word: 0/1 = 8-bit unsigned mono/stereo, 2/3 = 16-bit
    signed mono/stereo, 4 = 8-bit VIDC log, 8 = 4-bit ADPCM, >19 = offset to a
    Win32 `WAVEFORMAT` block; pitch is the actual sample rate in Hz.

Reconstructing the audio means running the `DIR1` event sequencer against the
SoundLib samples (mixing per-frame), which is a separate, larger task than the
video decode and well below it in priority — the video frames are the goal.

## Implementation plan

1. **Container split** — reuse `replay_ae7` to get each chunk's ACEF payload
   (works today). Parse the ACEF header (dims, bpp, frame count) and locate the
   `PALE` palette and the frame table.
2. **Frame decode** — a native `replay_tca` decoder: LZW (and RLE/raw) per frame,
   XOR-delta against the previous frame for `update` frames, into an 8bpp buffer.
   Start with the 8-bit-direct mode; add 4-bit/row-doubled modes as samples need.
3. **Colour** — apply the `PALE` palette; emit through the transcoder's existing
   `COL_PAL8` → RGB24 path (a new native dispatch like `direct_type23`, but
   stateful across frames).
4. **Audio** (optional, later) — IotaSound `WAV1`/`WAV2` from the `SOUN` chunk;
   separate from video and can follow.
5. **Validate** against `BUCCAN` (17 frames, 256×192, 8bpp) end to end.

## References

- Nihav `nihav-acorn`: `src/demuxers/tca.rs` (container), `src/codecs/euclid.rs`
  (LZW + frame decode), `src/codecs/iotasound.rs` (audio). Authoritative and
  readable; the C decoder should track these.
- Iota format spec — `http://www.iota.co.uk/tca/filefmt/format.txt` (the
  authoritative text), plus the overview `index78f5.html?page=filefmt/default`
  the PALE page `indexe466.html?page=filefmt/pale`, and the SoundLib spec
  `filefmt/soundlib.txt`. WebFetch can't reach the
  site (it forces HTTPS and the cert name is invalid); plain `curl` over HTTP from
  the sandbox **works** (`curl -ksS http://www.iota.co.uk/...`). The `FADE`/pauses
  page is a soft-404 (PHP `include` of a missing `filefmt/filefmt/pauses.htm`).
- `ARMovie_2003/Video/Decomp500/`: the `!RunImage` BASIC orchestration and the
  `EuclidX`/`IotaSound` modules — last-resort disassembly authority for the LZW.

## Implemented

`src/replay_tca.c` (`include/replay/replay_tca.h`) decodes the IotaFilm — ACEF
header, PALE palette, and the LZW/RLE/raw + Delta frame blocks — to 8bpp indices
+ an RGB palette, for the 8-bit modes (28/21). All screen modes are handled: 8-bit direct (28/21), 4-bit
nibble-unpacked full height (27), and the half-height vertically-doubled 4-bit
(12/13/39) and 8-bit (15/36/40) modes — the packed buffer is decoded then
expanded to `width*height` indices. `replay-transcode` drives it via a native
`direct_tca` dispatch (`codec_info` case 500): the film is read from the first
chunk's offset, each frame goes through `COL_PAL8` → RGB24. Iota sound (codec
500) is not decoded, so a type-500 movie transcodes video-only (use
`--skip-unsupported` for the audio track). `test_replay_tca` covers the
raw/LZW/Delta and mode-27 (4-bit) paths on synthetic films; BUCCAN transcodes end
to end to the Buccaneer, and the `.tca` corpus (modes 27/12/15/28, Normal and
Delta) decodes correctly.

## Remaining work / open questions

- **Audio** (issue #35): Iota sound is event-driven (`DIR1` play events
  against the `SOUN` SoundLib `&C47`), not a linear PCM track — a separate,
  larger task (see the Audio section). Type-500 movies currently transcode
  video-only.
- **Multi-chunk** (issue #34): BUCCAN is a single chunk holding the whole
  ACEF. Whether type-500 movies are ever split across multiple Replay chunks (and
  if so how frames/PALE are distributed) is unconfirmed — it may not occur in
  practice; needs a multi-chunk sample.
- **RLE (technique 0)** is implemented from `euclid.rs` but untested — no corpus
  sample uses it (all are LZW).
