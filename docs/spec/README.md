# Acorn Replay format specifications

Authoritative, implementation-grade specifications for the Acorn Replay
(ARMovie) bitstream and container formats. Each document is meant to be
sufficient, on its own, to build a conforming encoder, decoder or muxer without
reading the reference C.

These differ from the working notes in [`../../notes/`](../../notes/): the notes
record how each format was *investigated* (often reasoning forward from Acorn's
BASIC compressor source); these specs record what the format *is*, as verified
byte-for-byte against the shipped Acorn decoders. Where the two disagree, the
spec is correct and its provenance appendix explains why.

## Documents

| Spec | Covers | Status |
| --- | --- | --- |
| [methodology.md](methodology.md) | Sources, verification method, and the conventions (bit order, colour) every other spec relies on. Read this first. | stable |
| [type19-super-moving-blocks.md](type19-super-moving-blocks.md) | Compression type 19, *Super Moving Blocks* — the flagship Moving Blocks bitstream (6Y5UV). | complete |
| [moving-blocks-7-17-20.md](moving-blocks-7-17-20.md) | The other Moving Blocks codecs — types 7, 17 and 20 — as deltas from type 19. | complete |
| [ae7-armovie-container.md](ae7-armovie-container.md) | The ARMovie/AE7 container: text header, chunk catalogue, frame and sound layout. | complete |
| [armovie-sound.md](armovie-sound.md) | The ARMovie sound formats: VIDC exponential / signed linear (format 1) and IMA/DVI ADPCM (format 2). | complete |
| [raw-formats-2-23.md](raw-formats-2-23.md) | The uncompressed video formats — type 2 (16-bit raw) and type 23 (4:2:2 6Y6Y5U5V). | complete |
| [type1-moving-lines.md](type1-moving-lines.md) | Compression type 1, *Moving Lines* — the line-based temporal/spatial codec. | verified (`codec_movinglines.c`, cross-checked vs the compiled module) |
| [tca-type500.md](tca-type500.md) | Type 500, Iota *"The Complete Animator"* (TCA/ACEF) — native decoder + Iota sound. | implemented |
| [eidos-escape.md](eidos-escape.md) | The Eidos *Escape* family (types 100–199): version timeline and frame formats. Type 130 (Escape 2.0) is implemented via FFmpeg pass-through. | 130 implemented; 122 needs a native decoder |

### Shared subsystems

| Spec | Covers | Status |
| --- | --- | --- |
| [colour-pipeline.md](colour-pipeline.md) | RGB↔6Y*n*UV: CompLib constants, ARM-ASR rounding, ordered dither, the display inverse. | complete |
| [quality-model.md](quality-model.md) | The encoder-side 29-row QP% copy-acceptance table and test. | complete |

## Outstanding

Every Acorn video type the project knows (1, 2, 7, 17, 19, 20, 23) plus the
container and sound is now specified and verified, with a codec behind each.

**Moving Lines colour is confirmed on real hardware.** `replay-encode --codec 1`
muxes complete `.ae7` movies — in both colour models, with sound — that play
correctly on the shipped ARPlayer under RPCEmu. The colour metadata is pinned
from the player source: it packs RGB555 **red-in-low-bits** (the `bgr555le` /
Replay-poster convention) at 16-bit depth; YUV is labelled `YUV` so `bas/Player`
selects `YUV16`, while RGB is written label-less and relies on the player's `rgb`
default (`ColourMap.RGB16`), which also avoids ARPlayer's cosmetic `[` display
(see [player-bugs.md](../player-bugs.md) #6).

What remains is optional polish:

- **From-assembler colour rounding.** The one open numerical detail noted in
  [colour-pipeline.md](colour-pipeline.md): confirming CompLib's real-to-integer
  rounding at coefficient boundaries straight from the assembler.

## How to read a spec

Every spec is built from the same parts:

1. **Identity** — the codec number, name, and the `Resources/Info` declaration.
2. **Normative format** — byte- and bit-exact layout. All multi-bit fields are
   read **least-significant bit first** (see methodology.md); a field's value is
   the integer the decoder accumulates, not the order bits appear on the wire.
3. **Encoder-only material** — decisions an encoder must make that a decoder
   never sees (e.g. copy-acceptance thresholds), clearly marked *(encoder)*.
4. **Provenance & corrections** — an appendix citing the original Acorn sources
   and listing every place those sources were wrong, ambiguous, or silent, with
   how the correct behaviour was established.
