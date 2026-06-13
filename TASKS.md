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
- [x] Compare portable and original-compressor decisions and quantify bitrate
  and decoder-visible quality differences; exact decision parity is not a goal.
- [x] Add source-independent aggregate mode and bit accounting for original and
  portable type 19, Super Moving Blocks payloads.
- [x] Decode candidate type 7, Moving Blocks source movies with compiled
  Decomp7 and reject mismatched provenance using native-domain quality checks.
- [x] Establish `LionFish2,ae7` as the exact source and capture its authoritative
  uncompressed word stream through the type 2 intermediate.
- [x] Add per-block decoder traces and native 6Y5UV quality metrics.
- [x] Implement and measure a cross-family lowest-error policy, with emitted
  bits and stable table order as documented tie-breakers.
- [x] Add a reproducible fixed-level policy sweep with sequence quality and
  bitrate aggregation.
- [x] Add bounded adjacent-first bracketed target search and compare both
  policies at a matched 6,000-byte target over the 25-frame corpus.
- [x] Cache temporal motion profiles across target-byte retries, while keeping
  reconstruction-dependent spatial searches live.
- [x] Extract all 375 native frames from the validated type 2 `LionFishX,ae7`
  intermediate for full-movie sweeps.
- [ ] Add an exact `--policy acorn` mode if later decision traces justify and
  define behavior beyond the existing `lowest-error` and `ordered` policies.

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

- [x] Write a Replay/AE7 container (`replay_ae7_write` + `replay-join`),
  reproducing Join's layout: 21-line header, sector-aligned chunks, even/odd
  buffer sizes, time-sliced interleaved sound, configurable chunk size and
  alignment, optional key-frame area. Field requirements cross-checked against
  Acorn's `Join` source.
- [x] Parse the generated container back and verify every payload: a round-trip
  unit test plus an end-to-end ffmpeg->encode->join run whose chunk-0 video
  region is decoded by the compiled Decomp19 (12 frames, 0 trailing bytes).
- [x] Confirm playback on real RISC OS: type 19 video + VIDC-E8 sound plays in
  both the command-line Player and the !ARPlayer GUI.
- [x] Record the player-compatibility constraints (see
  `notes/replay-ae7-join-writer.md`): audio movies need >=2 chunks (a one-chunk
  movie aliases the player's double buffers), sound-only movies need 0x0
  dimensions, frames-per-chunk >= 3, and !ARPlayer requires an embedded poster
  sprite (16bpp square-pixel mode word); the writer enforces all of these and
  embeds the Replay-logo default poster when none is supplied.
- [x] Add a one-shot `tools/replay-make` driver (ffmpeg -> encode -> join).
- [x] Add a signed-16-PCM -> Replay sound encoder (`replay_sound`): VIDC 8-bit
  exponential (nearest-match inverse of Acorn's exact `ELogToLinTable`, ~37 dB
  round-trip SNR), plus 8-bit and 16-bit signed linear. Wired into `replay-join`
  as `--sound-pcm`/`--sound-encode`, fed canonical `s16le` from ffmpeg.
- [x] Add a PCM -> IMA ADPCM encoder and the per-chunk sound-encoding path it
  needs (4-byte state header per chunk): replay-join/replay-make --sound-encode
  adpcm (format 1 SoundA4) or adpcm2 (format 2 "2 adpcm"). Mono; both flavours
  emit identical bytes and differ only in the header. (Stereo ADPCM and other
  framed format-2 codecs remain future work.)
- [x] Generate type 19 per-chunk key frames (reconstruction packed as 6Y5UV
  halfwords) so the player can start at any chunk: replay-encode --keys-prefix
  emits one per frame, the writer selects the chunk boundaries (chunk_count-1
  blocks) and replay-join/replay-make expose --keys-prefix/--keys.

## Milestone 8: Modern Input And Uncompressed Formats

- [x] Accept raw RGB24 from an FFmpeg pipe.
- [x] Document and test FFmpeg commands, frame sizing, aspect handling, EOF,
  and pipe error propagation.
- [ ] Decide whether direct libavformat/libavcodec integration adds enough
  value over the raw pipe to justify an optional dependency.
- [x] Implement type 23, 6Y6Y5U5V packed 4:2:2 frame packing, unpacking, and
  AE7 extraction.
- [x] Verify the exact type 23 packing against the compiled Acorn decompressor.
- [ ] Evaluate types 8 and 21 for direct FFmpeg RGB24/YUV24/YUYV interchange.
- [ ] Keep type 2 `type19-fields` as an explicit reinterpretation; do not
  silently present it as RGB555, YUV555, or general 6Y5UV conversion.
- [ ] Investigate the unplayable output produced by the Acorn compressor's
  `6YVUV` colour-space selection before copying that conversion path.

## Next Codec Backends

- [x] Add and compiled-decoder cross-check type 17, Moving Blocks HQ, Huffman
  and data-coded 4x4/2x2 primitives through `codec_movingblockshq`.
- [x] Complete type 17 stationary, temporal, spatial, split, and strict frame
  verification through the shared `mb_frame_verify` grammar.
- [ ] Implement the type 17 encoder and measure its quality/bitrate behavior.
- [ ] Add type 7, Moving Blocks, through `codec_movingblocks`.
- [ ] Add type 20, Moving Blocks Beta, through `codec_movingblocksbeta`.
- [ ] Add Moving Lines as a separate codec core sharing only general tooling.
