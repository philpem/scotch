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

## Container format (ACEF), from Nihav `demuxers/tca.rs`

- Optional `ACEF` tag + `u32le` size, then a ~0x30-byte header; width @ +0x08,
  height @ +0x0C of the header (`u32le`); even dimensions required. (Header also
  carries a NUL/CR-terminated title and the bpp — see the BUCCAN dump above; exact
  offsets to be pinned during implementation.)
- **Frames** follow, each prefixed with a `u32le` size (payload = `size - 4`;
  valid range 9..1048576).
- **Trailing chunks** scanned by `tag` + `u32le size`:
  - `PALE` — palette. Size `0x28..0x428`, multiple of 4; entries are
    `[index:u8][R:u8][G:u8][B:u8]` → up to 256 RGB triplets (768-byte palette).
  - `RATE` — frame rate (`tb_num`, `tb_den`; `tb_den` doubled).
  - `SOUN` — audio, with a nested `WAV1`/`WAV2` tag (Iota sound).
  - `DIR1`, `FULL` — present in BUCCAN (per the `!RunImage` chunk search); roles
    TBD (likely a frame directory / full-frame index).

## Frame decode, from Nihav `codecs/euclid.rs`

- Per-frame **method**: `0 = RLE`, `1 = LZW`, `2 = raw`.
- **`update` (delta) flag**: when set, the decoded bytes are XORed into the
  previous frame (`*pix ^= b`) — i.e. predicted/P-frame; otherwise a keyframe.
- **LZW**: variable-width, `START_BITS=9`, `MAX_BITS=16`, dictionary starts at
  `START_POS=257`; code `256` is the clear/end code; dictionary grows
  `dict_lim <<= 1; idx_bits += 1` up to 16 bits; `dict_sym[]`/`dict_prev[]`
  backlink arrays, emitting each code's bytes in reverse (standard LZW).
- **Pixel layout is screen-mode dependent**: 4-bit-packed half-res
  (modes 12/13/39), row-doubled (15/36/40), 4-bit full height (27), 8-bit direct
  (21/28). BUCCAN is 8bpp, so the **8-bit-direct** path is the first target.
- **Output**: 8-bit paletted (`PAL8`) + the 768-byte palette from `PALE` → our
  pipeline converts to RGB24 via the existing `COL_PAL8` path.

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
- Iota format spec: `http://www.iota.co.uk/tca/filefmt/format.txt` plus the ACEF
  and PALE chunk pages. The server forces HTTPS with a mismatched certificate, so
  WebFetch can't reach it; fetch locally with `curl -k`.
- `ARMovie_2003/Video/Decomp500/`: the `!RunImage` BASIC orchestration and the
  `EuclidX`/`IotaSound` modules — last-resort disassembly authority for the LZW.

## Open questions

- Exact ACEF header layout (title length handling, where bpp/mode/frame-count and
  the per-frame `method`/`update` flags live) — pin from `euclid.rs` + a hexdump
  walk of BUCCAN's frames.
- Which screen `mode` BUCCAN's frames declare (drives the pixel layout path).
- Whether frames are individually size-prefixed within the chunk or indexed via a
  `DIR1`/`FULL` table.
- Iota sound (`SOUN`/`WAV1`/`WAV2`) format for the audio track.
