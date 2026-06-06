# Type 19, Super Moving Blocks Implementation Status

This document separates implemented behavior, verified compatibility, and
known gaps. Source descriptions live in the project-level `notes` directory;
this file describes the current portable code in `replay-tooling`.

## Implemented

- Growable byte buffers and LSB-first bit readers/writers.
- Source-derived type 19, Super Moving Blocks Huffman, temporal-motion,
  spatial-motion, and
  29-level quality tables.
- Full type 19, Super Moving Blocks payload verification: 4x4 data, stationary,
  temporal, spatial,
  and split 2x2 forms.
- RGB24 to CompLib-style non-dithered `6Y5UV` conversion.
- Deterministic type 19, Super Moving Blocks encoding with reconstructed-frame
  feedback.
- Stationary, temporal, and spatial matching at loss levels `0..28`.
- Real bit-cost comparison between 4x4 data and a four-quadrant split.
- Whole-frame target-byte retries using configurable floating-point window
  factors; defaults are `0.90` and `1.025`, explicitly truncated to bytes.
- Raw RGB24 input from a file or FFmpeg pipe, numbered payload output, traces,
  and reconstructed PPM output.
- Packed `Y,U,V` corpus import/export and first-pixel comparison diagnostics.
- Per-block decode traces with exact bit ranges and motion vectors.
- Native 6Y5UV SSE/MSE, PSNR, and maximum-error metrics in encoder traces and
  verifier comparisons.
- Unicorn execution of the compiled Acorn Decomp19 binary through `CodecIf`.
- Bounded AE7 header and chunk-catalogue parsing plus a `replay-inspect` CLI.
- Sequential ARM decoding that uses returned source pointers to split Replay
  chunks into exact frame payloads.

## Verified Claims

- Hand-reviewed golden tests cover bit order, Huffman symbols, data blocks, and
  all block-mode decoder paths.
- Every encoder attempt is decoded independently and compared with the
  encoder's reconstructed frame before output is accepted.
- Acorn's compiled `Decompress,ffd` and the portable verifier produce identical
  `6Y5UV` for focused stationary, temporal, spatial, split, and lossy fixtures,
  including both 4x4 and motion-coded 2x2 paths.
- The same decoders agree byte-for-byte on all 25 original-compressor frames
  in chunk 0 of `LionFish19,ae7`; the first two are permanent corpus fixtures.
- Normal and ASan/UBSan test suites cover the C implementation; Unicorn tests
  run when its Python bindings and the compiled decoder are available.

## Deliberate Policy Choices

- The codec core performs one deterministic pass. Target-size retry policy is
  in shared `mb_rate_control` code.
- Top-level copy-mode priority is stationary, temporal, then spatial. Temporal
  and spatial searches retain the lowest accepted error, with table order as
  the tie-break. This is compatible stream generation; Acorn decision parity
  is not required, but its bitrate and quality effects must be measured and
  documented.
- A 4x4 data candidate wins a bit-cost tie with a split candidate.
- Raw payload sequences are separate files; no undocumented temporary
  container is invented.

## Known Gaps

- CompLib RGB conversion constants are source-derived but not yet compared
  byte-for-byte with an ARM conversion fixture.
- Acorn's chunk-budget carry and three-level `Cut` escape are not implemented;
  they require real Replay container/chunk accounting.
- There is no AE7/Replay container writer or player acceptance test. The
  reader currently exposes chunk boundaries; type 19 frame splitting remains
  a decoder-assisted operation because AE7 stores no per-frame size table.
- Formats 7, 17, and 20 have descriptors and notes but no complete portable
  encoder/decoder cores: type 7, Moving Blocks; type 17, Moving Blocks HQ; and
  type 20, Moving Blocks Beta. Moving Lines remains separate future work.
