# Replay type 500 — Iota "The Complete Animator" (TCA / ACEF)

Scoping for Replay video format **500**, the second codec called out in issue #9
(the "All About Planes" educational disc; sample `test-videos/BUCCAN`). Status:
*scoping/RE — not yet implemented.*

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
| `DIR1` | 78404 | 160 | sound-effect soundtrack |
| `SOUN` | 78564 | 44296 | SoundLib samples (`&C47`) |
| `FADE` | — | — | fades/pauses between frames — **doc 404s** (Iota's `pauses.htm` is gone); optional, ignore |

So decode = AE7 reader gives `chunk[0].file_offset`; from there, `FNfilm_findchunk`
for `ACEF` (frames) and `PALE` (palette). (`RATE` is redundant with the Replay
header's fps; `SOUN`/`DIR1` are audio, later.)

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

### LZW (technique 1), from Nihav `codecs/euclid.rs`

Variable-width: `START_BITS=9 .. MAX_BITS=16`, dictionary from `START_POS=257`,
code `256` = clear/end; width grows (`dict_lim <<= 1; idx_bits += 1`) up to 16
bits; `dict_sym[]`/`dict_prev[]` backlink arrays, each code emitted in reverse —
standard LZW. (RLE = technique 0; exact bit order / RLE opcodes to confirm against
`euclid.rs` while implementing.)

## PALE chunk (from Iota PALE page, validated on BUCCAN)

`[id:"PALE"][size:u32le]` then words: `+8` pencil colour, `+12` paper colour,
`+16` ModeFlags (bit 7 = Acorn "true" 256-colour palette), `+20` Log2BPP
(`3 = 8bpp`), `+24` Log2BPC, `+28` XEigFactor, `+32` YEigFactor, `+36..` the
palette: one **ColourTrans** word per logical colour. On disk each word is
`[index:u8][R:u8][G:u8][B:u8]` (LE) — verified on BUCCAN: idx0=`00 00 00 00`
(black), idx4=`04 44 00 00` (R=0x44). So `R=byte1, G=byte2, B=byte3`; BUCCAN has
256 entries (size 1060 = 36 + 256×4). Build a 256×3 RGB table → `COL_PAL8`.

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
  and the PALE page `indexe466.html?page=filefmt/pale`. WebFetch can't reach the
  site (it forces HTTPS and the cert name is invalid); plain `curl` over HTTP from
  the sandbox **works** (`curl -ksS http://www.iota.co.uk/...`). The `FADE`/pauses
  page is a soft-404 (PHP `include` of a missing `filefmt/filefmt/pauses.htm`).
- `ARMovie_2003/Video/Decomp500/`: the `!RunImage` BASIC orchestration and the
  `EuclidX`/`IotaSound` modules — last-resort disassembly authority for the LZW.

## Open questions (remaining)

- Exact **LZW bit order** (LSB- vs MSB-first) and the **RLE (technique 0)** opcode
  format — pin from `euclid.rs` (and a frame-0 byte walk) while implementing.
- **Mode → screen layout**: BUCCAN is mode 28 (8bpp); confirm row word-alignment
  and whether multi-byte modes (16/24bpp) appear in real type-500 movies.
- **Multi-chunk** type-500 layout: BUCCAN is a single chunk holding the whole
  ACEF. How a multi-chunk movie splits frames across Replay chunks (and whether
  PALE/SOUN are stored once) needs a second sample to confirm.
- Iota sound (`SOUN` SoundLib `&C47` / `DIR1`) format for the audio track.
