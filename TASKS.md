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
- [ ] Implement matching data-block writers only after the golden decoder tests
  pass.

## Milestone 3: Complete Format-19 Payload Verifier

- [ ] Decode stationary 4x4 and 2x2 blocks.
- [ ] Decode temporal motion codes against a supplied previous frame.
- [ ] Decode legal spatial copies against the current reconstructed frame.
- [ ] Decode split 2x2 blocks and validate frame scan completion.
- [ ] Verify complete data-only synthetic frames.
- [ ] Add malformed-stream tests for every opcode family.

## Milestone 4: Independent Acorn Cross-Check

- [ ] Build a small corpus of original format-19 Replay payloads, preserving
  provenance and expected dimensions.
- [ ] Decode the corpus with the portable verifier and export raw `6Y5UV`.
- [ ] Create a RISC OS harness that invokes the original Decomp19 entry point.
- [ ] Run the harness in an emulator and capture raw decoded pixels.
- [ ] Compare portable and Acorn output byte-for-byte.
- [ ] Keep any compatibility quirks as named regression fixtures.

## Milestone 5: Data-Only Encoder

- [ ] Implement CompLib-compatible RGB24 to `6Y5UV` conversion and tests.
- [ ] Read exact-sized raw RGB24 frames from files or stdin.
- [ ] Encode complete frames using only data-coded blocks.
- [ ] Decode every emitted frame and compare it with encoder reconstruction.
- [ ] Add reconstructed-frame PPM output and decision traces.

## Milestone 6: Compression Modes

- [ ] Add stationary candidates and cross-frame state.
- [ ] Add temporal motion candidates.
- [ ] Add spatial candidates.
- [ ] Add 4x4 versus split-2x2 bit-cost selection.
- [ ] Add the 29-level threshold table and fixed-loss tests.
- [ ] Add target-byte retries after deterministic single-pass encoding works.

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
