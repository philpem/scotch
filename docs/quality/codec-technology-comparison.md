# Moving Blocks vs. contemporaneous codecs

How the Acorn Replay Moving Blocks family works, and where it sits among the
video codecs of its era (roughly 1991–1996). Codec internals here are drawn from
`notes/moving-blocks-variants.md` and the per-format bitstream notes; the
comparison codecs are characterised from their well-known designs.

## The Moving Blocks family

All four share one design and differ mostly in colour precision and entropy
coding. The shared model:

- Frame scanned as **4×4 blocks**, top-left to bottom-right.
- The encoder builds the **reconstructed** frame as it goes; temporal matches are
  made against that reconstruction, not the original — so the encoder and the
  player's decoder never drift apart.
- Each block picks one of: **raw data**, **temporal copy** (motion vector into
  the previous decoded frame), **spatial copy** (from already-decoded pixels in
  the current frame), or **split into four 2×2** sub-decisions.
- A `QP%` ("quality") table sets match tolerances; raising it loosens matching,
  shrinks output and lowers fidelity. The whole frame is re-encoded at different
  `QP%` rows to hit a byte budget (rate control).

No DCT, no wavelet, no vector-quantisation codebook. It is **block-copy +
optional entropy coding of raw samples** — deliberately cheap to decode on an
ARM2/ARM3 Archimedes.

| Type | Name | Date | Colour | Motion | Entropy coding | Distinctive |
|---:|---|---|---|---|---|---|
| 7  | Moving Blocks       | 1993 (fmt) | YUV 5,5,5 | ±4 | none — fixed 90-bit 4×4 / 30-bit 2×2 | no explicit stationary code (uses a (0,0) copy) |
| 17 | Moving Blocks HQ    | Aug 1996 | YUV 5,5,5 | ±8 | 9-bit Huffman on samples | explicit stationary cases; 2-D luma predictor |
| 19 | Super Moving Blocks | Sep 1996 | 6Y5UV 6,5,5 | ±8 | 128-entry / 11-bit Huffman | 6-bit luma; `testcount` 2×2-vs-4×4 cost test |
| 20 | Moving Blocks Beta  | Nov 1996 | 6Y6UV 6,6,6 | ±8 | 128-entry / 11-bit Huffman | 6-bit chroma; `D4tab` delta table + predictive prev-U/V chroma |

The progression is: add explicit "nothing changed" cases (17), widen motion
search (17), add Huffman entropy coding of raw samples (17→19→20), raise colour
precision (19 luma, 20 chroma), and add predictive chroma (20). Each step buys
compression efficiency for a little more decode cost.

## Where it sits among 1990s codecs

The defining axis of the era was **transform vs. non-transform**:

- **Transform codecs** (DCT) — MPEG-1, H.261, Motion-JPEG — get much better
  compression but need an 8×8 inverse DCT per block, which was expensive on
  period desktop CPUs without dedicated hardware.
- **Block / VQ codecs** — Apple Video (RPZA), Microsoft Video 1, Cinepak, Indeo
  3 — trade compression for very cheap, integer-only software decode. This is
  the family Moving Blocks belongs to.

| Codec | ~Year | Class | Colour | Inter-frame | Entropy coding | Decode cost |
|---|---|---|---|---|---|---|
| **Moving Blocks (7)** | 1993 | block copy | 15-bit YUV | motion + spatial + temporal copy vs. *reconstructed* frame | none | very low |
| **Moving Blocks HQ/Super/Beta (17/19/20)** | 1996 | block copy | 15–18-bit YUV | as above | Huffman | low |
| Apple Video / RPZA | 1991 | block (BTC-ish) | 15-bit RGB | skip blocks | none | very low |
| Microsoft Video 1 (MS-CRAM) | 1992 | block (2/8-colour) | 15-bit RGB | skip blocks | none | very low |
| Cinepak | 1991–92 | vector quantisation | YUV (codebook) | block skip + codebook reuse | none | low |
| Intel Indeo 3 | 1993 | VQ + motion | YUV | motion vectors | none | low–medium |
| Smacker | 1994 | block + entropy | 8-bit palette | block-based deltas | custom Huffman trees | low |
| H.261 | 1990 | DCT 8×8 | 4:2:0 | half-pel motion | Huffman | high |
| MPEG-1 video | 1993 | DCT 8×8 | 4:2:0 | I/P/B, half-pel motion | Huffman | high |
| Motion-JPEG | early 90s | DCT 8×8 | 4:2:0/4:2:2 | none (intra only) | Huffman | medium |

### Closest relatives

- **Apple RPZA and Microsoft Video 1** are the nearest cousins: 4×4 blocks,
  15-bit colour, per-block mode selection, skip/copy for static regions, no
  transform. Moving Blocks goes further with **motion-compensated** temporal
  copies (RPZA/MS Video 1 only "skip" a block in place) and **spatial** copies,
  and the 1996 variants add Huffman coding those two never had.
- **Cinepak / Indeo 3** reach better compression via vector quantisation
  (a per-clip codebook of small blocks). Moving Blocks uses no codebook — it
  codes raw blocks directly or copies them — which is simpler to encode and
  decode but generally less efficient at very low rates.

### Versus the DCT codecs

MPEG-1 / H.261 / M-JPEG compress far better at a given quality, especially on
detailed natural video, because the DCT decorrelates spatial detail and quantises
perceptually. Moving Blocks has no transform, so high-detail blocks must be sent
as raw samples (its most expensive case). The trade was deliberate: Replay had to
decode in software on Acorn hardware, where a per-block IDCT was too costly. The
family's strengths — flat/static regions (cheap stationary + copy cases) and
smooth motion (cheap temporal copies) — are exactly what its copy-based model
handles well, and where the rate/quality curves in this directory are most
competitive.

## The two Moving Blocks Beta revisions (Sep'96 vs Nov'96)

"Type 20" is not one format — there are **two shipped Decomp20 revisions** with
different chroma coding. Full detail and the byte-level evidence are in
`docs/type20-shipped-vs-source.md`; the essentials:

| | Beta "old" (Sep'96) | Beta "new" (Nov'96) |
|---|---|---|
| Decoder module | `Decompress,ffd` 12456 bytes, **20 Sep 1996** | `Decompress,ffd` 12528 bytes, **19 Nov 1996** |
| Version string | v0.04, 6 Sep 1996 | v0.05, 19 Nov 1996 |
| Ships in | `!Boot/Resources/!ARMovie` | `Replay/Replay3`, `Replay/OldARM` |
| Chroma coding | **direct 6-bit** | **delta-coded** |
| Data-block header | opcode(2) + U(6) + V(6) = **14 bits** | opcode(2) + uv-byte(8) = **10 bits** |
| Our flag | `--variant old` (default) | `--variant new` (alias to type 30) |

**Everything else is identical:** the block grammar (same as type 19), the
format-19 motion tables, the 64-symbol luma Huffman table, the 2-D luma
predictor, and the 6Y6UV output word (`Y | U<<6 | V<<12`, six bits each). The
*only* difference is how a data block carries chroma:

- **Old (v0.04):** `U` and `V` stored directly as 6-bit signed-mod-64 fields. No
  predictor, no table.
- **New (v0.05):** an 8-bit byte holds two 4-bit delta codes (U in the low
  nibble, V in the high nibble) that index
  `deltaexpand = {-32,-26,-20,-14,-8,-4,-2,-1, 0,1,2,4,8,14,20,26}`. Each delta
  is added to a per-component chroma predictor carried across data blocks and
  reset to zero each frame (`u = (prevu + deltaexpand[code]) & 63`), exactly
  like the luma predictor. The encoder builds a `D4tab` (via `GenD4Table`) to
  pick the closest delta code. This is what trims a few bytes per frame.

### Are they bitstream compatible?

**No.** A stream encoded for one revision will not decode on the other. The
data-block header is **14 bits (old)** vs **10 bits (new)** — that 4-bit length
difference alone walks the bitstream pointer off position, so everything after
the first data block (including the luma that follows) decodes as garbage. They
are mutually unintelligible despite sharing ~all of the format.

Because an `.ae7` movie only declares "compression type 20", its bytes must
match whichever Decomp20 the player happens to have installed. That is exactly
why the tooling can **alias the new variant to a free type number** (`replay-make
--codec 20 --variant new --type-alias 30`): the new delta decoder installs at
`Decomp30` alongside the old `Decomp20`, so both kinds of movie play on one
machine without a clash. Each variant is cross-checked byte-for-byte against its
real module (`test_decomp20_compiled` for old, `test_decomp20new_compiled` for
new); the incompatibility was originally caught by tracing the bitstream pointer
under the Unicorn harness.

## Take-aways

- Moving Blocks is a **non-transform, copy-based, motion-compensated block
  codec** in the RPZA / MS Video 1 / Cinepak software-playback class, not the
  MPEG/DCT class.
- Its standout architectural choice is **predicting against the reconstructed
  frame** with a rich per-block mode set (raw / temporal-motion / spatial /
  split), which most of its block-codec peers lack.
- The 1996 revisions (17/19/20) modernised it with **Huffman entropy coding,
  wider motion search, higher colour precision, and predictive chroma**,
  narrowing the gap to the VQ codecs while staying cheap to decode.
