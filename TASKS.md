# Implementation And Cross-Check Tasks

## Milestone 1: Verification Foundations

- [x] Create an isolated CMake C project and Git repository.
- [x] Define naming and module boundaries.
- [x] Implement growable byte buffering.
- [x] Implement independent LSB-first bit writer and reader.
- [x] Define Moving Blocks codec descriptors for types 7, 17, 19, and 20.
- [x] Add the complete type 19, Super Moving Blocks luma Huffman table.
- [x] Implement shared table-driven Huffman write/decode helpers.
- [x] Test byte growth, bit ordering, truncation, and all 64 residual symbols.
- [x] Add source-derived golden byte tests that do not decode through the
  writer's implementation.
- [x] Add a verifier CLI for descriptor and Huffman-table checks.

## Milestone 2: Type 19, Super Moving Blocks Data Blocks

- [x] Define working `6Y5UV` pixel and predictor state.
- [x] Implement verifier decoding for one data-coded 4x4 block.
- [x] Implement verifier decoding for one data-coded 2x2 block.
- [x] Create hand-calculated golden payloads and reconstructed pixels.
- [x] Reject malformed/truncated blocks with bit positions.
- [x] Implement matching data-block writers only after the golden decoder tests
  pass.

## Milestone 3: Complete Type 19, Super Moving Blocks Payload Verifier

- [x] Decode stationary 4x4 and 2x2 blocks.
- [x] Decode temporal motion codes against a supplied previous frame.
- [x] Decode legal spatial copies against the current reconstructed frame.
- [x] Decode split 2x2 blocks and validate frame scan completion.
- [x] Verify complete data-only synthetic frames.
- [x] Add malformed/truncated stream tests and strict payload padding checks.

## Milestone 4: Independent Acorn Cross-Check

- [x] Inventory bundled Replay movies; all discovered video samples are type 7,
  Moving Blocks, so a type 19, Super Moving Blocks corpus must be generated or
  sourced separately.
- [x] Build a small corpus of original type 19, Super Moving Blocks Replay
  payloads, preserving
  provenance and expected dimensions.
- [x] Receive and checksum the Acorn-compressed `LionFish19,ae7` type 19,
  Super Moving Blocks comparison movie.
- [x] Parse `LionFish19,ae7` and extract its frame payloads and metadata.
- [x] Define packed `6Y5UV` corpus I/O, manifest conventions, and a comparison
  runner with previous-frame support.
- [x] Decode an original-compressor corpus with the portable verifier and
  export raw `6Y5UV`.
- [x] Create a Unicorn harness for the generated Decomp19 entry point and
  document its register and pixel-packing contract.
- [x] Run the compiled Decomp19 binary under Unicorn and capture raw decoded
  pixels for all currently emitted block-mode families.
- [x] Compare portable and Acorn output byte-for-byte for those fixtures.
- [x] Keep the compiled decoder's classic unaligned-`LDMIA` behavior as a named
  and documented harness regression.
- [x] Cross-check temporal, spatial, split, and lossy payloads, including
  temporal and spatial 2x2 modes.
- [ ] Compare portable and original-compressor decisions and quantify bitrate
  and decoder-visible quality differences; exact decision parity is not a goal.
- [x] Add per-block decoder traces and native 6Y5UV quality metrics.
- [ ] Add selectable `--policy portable|acorn` encoder policy after comparison
  data justifies and defines the Acorn-compatible behavior.

## Milestone 5: Data-Only Encoder

- [x] Implement the source-derived non-dithered CompLib RGB24 to `6Y5UV`
  conversion and focused tests.
- [ ] Confirm coefficient rounding against an Acorn-generated RGB conversion
  fixture.
- [x] Read one exact-sized raw RGB24 frame from a file or stdin.
- [x] Encode complete frames using only 4x4 data-coded blocks.
- [x] Decode every emitted frame and compare it with encoder reconstruction.
- [x] Add a frame-level decision trace.
- [x] Add reconstructed-frame PPM output for visual inspection.
- [x] Generalise raw input to a frame sequence and write explicitly numbered
  raw payloads without inventing an intermediate container.

## Milestone 6: Compression Modes

- [x] Add stationary 4x4 candidates and cross-frame reconstruction state;
  level 0 is exact and higher levels use source-derived thresholds.
- [x] Add temporal 4x4 search in canonical code-length/table order.
- [x] Add spatial 4x4 search, including use within key frames.
- [x] Complete 4x4 versus split-2x2 bit-cost selection, including data,
  stationary, temporal, and spatial 2x2 modes.
- [x] Add the 29-level threshold table and fixed-loss tests.
- [x] Add target-byte retries after deterministic single-pass encoding works.

## Milestone 7: Replay Container And Playback

- [ ] Write a minimal video-only Replay/AE7 container.
- [ ] Parse the generated container back and verify every extracted payload.
- [ ] Confirm playback using an existing RISC OS Replay player.
- [ ] Record the smallest accepted container and key-frame requirements.

## Later Codecs

- [ ] Add type 17, Moving Blocks HQ, through `codec_movingblockshq`.
- [ ] Add type 20, Moving Blocks Beta, through `codec_movingblocksbeta`.
- [ ] Add type 7, Moving Blocks, through `codec_movingblocks`.
- [ ] Add Moving Lines as a separate codec core sharing only general tooling.
