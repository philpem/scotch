# Task List

This list tracks the documentation and reverse-engineering work for Acorn
Replay video compression tooling.

## Current Focus

- [x] Read the Replay format and codec interface documents before digging into
  codec code.
- [x] Document how `Temporal` and `Spatial` codec capabilities affect Player,
  CompLib, key frames, and decompressor calls.
- [x] Document the shared `CompLib` batch-compression library at a high level.
- [x] Signpost simple `CompLib`-based compressor examples.
- [x] Build an inventory of Acorn-specific video formats from `AE7doc`,
  `Docs/Status`, and `Decomp*/Resources/Info`.

## Acorn Moving Lines

- [x] Identify all Moving Lines source and runtime paths in RO3.71 and RO2003.
- [x] Document Moving Lines code-word families and terminators.
- [x] Document the complete Moving Lines bitstream format.
- [x] Document the compressor decision process and parameters.
- [x] Document decompressor behavior, including temporal and spatial copy
  tables.

## Acorn Moving Blocks Family

- [x] Identify all Moving Blocks-family formats and versions.
- [x] Document `Decomp7` Moving Blocks first-pass compression model.
- [x] Document exact `Decomp7` Moving Blocks bitstream tables and opcodes.
- [x] Document first-pass comparison of `Decomp17`, `Decomp19`, and
  `Decomp20` Moving Blocks variants.
- [x] Document `Decomp17` Moving Blocks HQ.
- [x] Document `Decomp19` Super Moving Blocks.
- [x] Document `Decomp20` Moving Blocks Beta.
- [x] Compare how the Moving Blocks variants differ in colour space, block
  coding, motion/spatial search, quality controls, and output format.
- [x] Document the Moving Blocks algorithms used by the compressors and options
  for compatible C implementation improvements.
- [x] Document compressor parameters and their effects.

## Implementation Prep

- [x] Decide which Replay codec should be implemented first in portable C.
- [x] Draft a self-documenting C implementation plan for format 19.
- [x] Extend the implementation plan for future Moving Blocks formats and
  FFmpeg-based input.
- [x] Draft a staged C tooling implementation roadmap.
- [x] Document library/dependency choices for buffers, bitstreams, testing, and
  input video.
- [x] Document a verifier-first implementation strategy.
- [x] Define shared Moving Blocks codec descriptors for formats 7, 17, 19, and
  20.
- [x] Define an input-frame representation for compressor tooling.
- [x] Define raw stdin/file input, initially `rgb24` frames from FFmpeg.
- [x] Decide when to add optional libav/FFmpeg library input support.
- [x] Define a bitstream writer matching Replay's least-significant-bit-first
  stream conventions where applicable.
- [x] Define trace/debug output for explaining block decisions.

## Implementation Work

- [x] Build decoder/verifier tools and golden tests before extending the full
  encoder.
- [x] Implement a self-checking format-19 encoder with data, stationary,
  temporal, spatial, split, quality-threshold, and rate-retry paths.
- [x] Cross-check generated stationary, temporal, spatial, split, and lossy
  streams against the compiled Decomp19 ARM implementation under Unicorn.
- [x] Add focused temporal, spatial, split, and lossy ARM decoder fixtures.
- [x] Add per-block type 19, Super Moving Blocks traces and native 6Y5UV
  bitrate/quality comparison metrics.
- [x] Add original-compressor payloads and compare its decisions with the
  portable encoder, documenting bitrate and decoder-visible quality effects.
- [x] Add selectable `lowest-error` and `ordered` portable policies.
- [x] Add target-byte retry search and cache temporal candidate measurements
  across retries without caching reconstruction-dependent spatial searches.
- [ ] Consider a selectable `acorn` encoder policy only if later decision
  traces define a useful behavioral difference beyond the current policies.
- [x] Document and test FFmpeg raw-pipe recipes for modern input.
- [ ] Decide
  whether optional direct libav integration is worthwhile.
- [x] Add type 23, `6Y6Y5U5V`, packed 4:2:2 frame packing, unpacking, and AE7
  extraction. It stores two horizontal Y samples per shared U/V pair and has
  no vertical chroma subsampling.
- [x] Add type 17, Moving Blocks HQ, Huffman and data-block decoder primitives
  and cross-check them against compiled Decomp17.
- [x] Complete type 17, Moving Blocks HQ, frame dispatch and cross-check copy
  and split modes against compiled Decomp17.
- [~] Type 17 encoder: data blocks done (codec_movingblockshq_encode_data*);
  frame encoder with shared motion search is next.
- [x] Add the device-bandwidth rate driver (replay-make --data-rate ...).
- [ ] Add type 7, Moving Blocks, as the next standard Replay codec backend.
- [x] Add a Replay/AE7 container writer (`replay_ae7_write`, `replay-join`) and
  confirm playback of video + sound in both the command-line Player and the
  !ARPlayer GUI on real RISC OS. See `replay-ae7-join-writer.md` for the format,
  the player-compatibility constraints (>=2 chunks for audio, sound-only 0x0,
  fpc>=3), and the poster-sprite requirement.
- [x] Add a signed-16-PCM -> VIDC E8 (and 8/16-bit signed linear) sound encoder
  (`replay_sound`); wire it and a poster into `replay-join`.
- [x] Add a one-shot `replay-make` driver (ffmpeg -> encode -> join in one step).
- [x] Generate type 19 per-chunk key frames so !ARPlayer can seek: encoder
  --keys-prefix, writer boundary selection (chunk_count-1 blocks), join --keys.
- [x] Add a PCM -> IMA ADPCM (mono) encoder with the per-chunk state-header
  path: replay-join/replay-make --sound-encode adpcm (SoundA4) or adpcm2.
- [ ] Investigate the original compressor's unplayable `6YVUV` output before
  reproducing that historical colour-space path.

## Parked / Not Planned

- RO3.71/RO2003 source comparison: parked unless a specific behaviour
  mismatch needs source-version archaeology.
- H.263 / `Decomp18`: parked for now because it is an industry-standard
  codec with separate documentation and source.
