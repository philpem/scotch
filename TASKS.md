# Implementation And Cross-Check Tasks

## Milestone 1: Verification Foundations

- [x] Create an isolated CMake C project and Git repository.
- [x] Define naming and module boundaries.
- [x] Implement growable byte buffering.
- [x] Implement independent LSB-first bit writer and reader.
- [x] Define Moving Blocks codec descriptors for types 7, 17, 19, and 20.
- [x] Add the complete format-19 luma Huffman table.
- [x] Implement shared table-driven Huffman write/decode helpers.
- [x] Test byte growth, bit ordering, truncation, and all 64 residual symbols.
- [x] Add source-derived golden byte tests that do not decode through the
  writer's implementation.
- [x] Add a verifier CLI for descriptor and Huffman-table checks.

## Milestone 2: Format-19 Data Blocks

- [x] Define working `6Y5UV` pixel and predictor state.
- [x] Implement verifier decoding for one data-coded 4x4 block.
- [x] Implement verifier decoding for one data-coded 2x2 block.
- [x] Create hand-calculated golden payloads and reconstructed pixels.
- [x] Reject malformed/truncated blocks with bit positions.
- [x] Implement matching data-block writers only after the golden decoder tests
  pass.

## Milestone 3: Complete Format-19 Payload Verifier

- [x] Decode stationary 4x4 and 2x2 blocks.
- [x] Decode temporal motion codes against a supplied previous frame.
- [x] Decode legal spatial copies against the current reconstructed frame.
- [x] Decode split 2x2 blocks and validate frame scan completion.
- [x] Verify complete data-only synthetic frames.
- [x] Add malformed/truncated stream tests and strict payload padding checks.

## Milestone 4: Independent Acorn Cross-Check

- [x] Inventory bundled Replay movies; all discovered video samples are type 7,
  so a format-19 corpus must be generated or sourced separately.
- [ ] Build a small corpus of original format-19 Replay payloads, preserving
  provenance and expected dimensions.
- [x] Define packed `6Y5UV` corpus I/O, manifest conventions, and a comparison
  runner with previous-frame support.
- [ ] Decode the corpus with the portable verifier and export raw `6Y5UV`.
- [x] Create a Unicorn harness for the generated Decomp19 entry point and
  document its register and pixel-packing contract.
- [x] Run the compiled Decomp19 binary under Unicorn and capture raw decoded
  pixels for data and stationary frames.
- [x] Compare portable and Acorn output byte-for-byte for those fixtures.
- [ ] Keep any compatibility quirks as named regression fixtures.

## Milestone 5: Data-Only Encoder

- [x] Implement the non-dithered CompLib RGB24 to `6Y5UV` conversion and
  focused tests; confirm coefficient rounding against an Acorn reference.
- [x] Read one exact-sized raw RGB24 frame from a file or stdin.
- [x] Encode complete frames using only 4x4 data-coded blocks.
- [x] Decode every emitted frame and compare it with encoder reconstruction.
- [x] Add a frame-level decision trace.
- [x] Add reconstructed-frame PPM output for visual inspection.
- [x] Generalise raw input to a frame sequence and write explicitly numbered
  raw payloads without inventing an intermediate container.

## Milestone 6: Compression Modes

- [x] Add exact stationary 4x4 candidates and cross-frame reconstruction
  state.
- [x] Add exact temporal 4x4 candidates in canonical code-length/table order.
- [x] Add exact spatial 4x4 candidates, including use within key frames.
- [x] Complete exact 4x4 versus split-2x2 bit-cost selection, including data,
  stationary, temporal, and spatial 2x2 modes.
- [x] Add the 29-level threshold table and fixed-loss tests.
- [x] Add target-byte retries after deterministic single-pass encoding works.

## Milestone 7: Replay Container And Playback

- [ ] Write a minimal video-only Replay/AE7 container.
- [ ] Parse the generated container back and verify every extracted payload.
- [ ] Confirm playback using an existing RISC OS Replay player.
- [ ] Record the smallest accepted container and key-frame requirements.

## Later Codecs

- [ ] Add type 17 through `codec_movingblockshq`.
- [ ] Add type 20 through `codec_movingblocksbeta`.
- [ ] Add type 7 through `codec_movingblocks`.
- [ ] Add Moving Lines as a separate codec core sharing only general tooling.
