# Replay Tooling Implementation Status

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
- Selectable `lowest-error` and legacy `ordered` copy-family policies;
  lowest-error is the command-line default.
- Stationary, temporal, and spatial matching at loss levels `0..28`.
- Real bit-cost comparison between 4x4 data and a four-quadrant split.
- Whole-frame target-byte retries using configurable floating-point window
  factors; defaults are `0.90` and `1.025`, explicitly truncated to bytes.
- Selectable linear and adjacent-first bracketed quality-row searches for
  target-byte retries.
- Raw RGB24 input from a file or FFmpeg pipe, numbered payload output, traces,
  and reconstructed PPM output. Direct libavformat/libavcodec integration is
  not implemented; the supported FFmpeg path is currently a raw-video pipe.
- Packed `Y,U,V` corpus import/export and first-pixel comparison diagnostics.
- Per-block decode traces with exact bit ranges and motion vectors.
- Native 6Y5UV SSE/MSE, PSNR, and maximum-error metrics in encoder traces and
  verifier comparisons.
- Unicorn execution of the compiled Acorn Decomp19 binary through `CodecIf`.
- Bounded AE7 header and chunk-catalogue parsing plus a `replay-inspect` CLI.
- Sequential ARM decoding that uses returned source pointers to split Replay
  chunks into exact frame payloads.
- Direct packed-6Y5UV encoder input and aggregate mode/bit/quality reports for
  encoder-policy comparisons.
- Reproducible multi-policy, multi-quality sweep tooling that preserves every
  payload, reconstruction, trace, and verifier report.
- Strict fixed-frame extraction from type 2 (16 bit colour uncompressed) AE7
  movies, with explicitly named type 19 field reinterpretation.
- Compiled type 7 (Moving Blocks) source decoding, including raw output words
  and native 16-bit key-frame initialization.
- Source-derived type 23, 6Y6Y5U5V packed 4:2:2 packing and unpacking, plus
  fixed-frame AE7 extraction through explicit `--type23-layout 6y6y5u5v`.
- Type 23 unpacking cross-checked against Acorn's compiled Decomp23 under
  Unicorn, including the classic ARM unaligned-LDM behavior between 44-bit
  four-pixel groups.
- A tested FFmpeg RGB24 raw-pipe workflow with documented frame-rate, scaling,
  aspect-ratio, exact-size, EOF, and shell error-propagation requirements.
- Type 17, Moving Blocks HQ, source-derived 32-symbol luma Huffman table and
  independently callable data-coded 4x4 and 2x2 decoder primitives. The
  primitives consume the complete case header, reconstruct five-bit luma with
  the codec's two-dimensional predictor, and update the shared predictor to
  the truncated block mean.
- An AE7/Replay container writer (`replay_ae7_write`, `replay-join`) that
  reproduces Acorn `Join`'s output: a 21-line text header with patched
  forward-reference offsets, optional sprite/key-frame areas, a text catalogue,
  and 2048-byte sector-aligned chunks each holding even-padded video followed by
  per-chunk time-sliced sound tracks. Chunk size is configurable as a fixed
  frame count or a target duration (fractional rates distribute frames as
  `floor((i+1)F) - floor(i*F)`), and the alignment mask is configurable.
  even/odd chunk sizes are `max(video+sound) per parity + 1`, matching Join.
  Sound-only movies (video format 0, no frames) and uncompressed video (any
  codec number, e.g. type 2 carrying raw 15-bit pixels) are both supported; the
  parser accepts zero dimensions when the video format is 0.
- Player-compatibility constraints learned from the v0.53 player source and
  emulator testing: an audio movie must have at least two chunks (a one-chunk
  movie aliases the player's double buffers and corrupts playback), a sound-only
  movie must report 0x0 dimensions, the catalogue always carries a sound field
  (";0" when silent), and frames-per-chunk must be >= 3. The writer enforces the
  first three; the encoder-side avoids the last. Verified end to end: a
  four-chunk 160x96 type 19 movie with 11025 Hz VIDC-E8 audio plays with sound
  in the RISC OS Replay player.
- !ARPlayer (GUI) reads the poster as spr_ReadSprite(sprite_offset+12,
  sprite_size-12) with no guard, so a movie with no sprite (size 0 -> a -12 byte
  read) crashes it; a GUI-loaded movie must embed a real RISC OS spritefile. The
  writer embeds one (replay-join --poster FILE.bgr555) and refuses video movies
  without one unless --no-poster is given. The poster is a 16bpp (1:5:5:5, red in
  the low bits) sprite with a new-format square-pixel mode word
  ((5<<27)|(90<<14)|(90<<1)|1); old numbered modes such as 28 are 8bpp. With no
  --poster the writer embeds the built-in Replay-logo Default sprite so the movie
  still opens in !ARPlayer.
- Type 19 key frames for chunk seeking: replay-encode --keys-prefix writes
  each reconstruction packed as 6Y5UV halfwords (Y[0:5] U[6:10] V[11:15]);
  the writer emits one key per chunk except the first (the end-of-chunk
  reconstruction, chunk_count-1 blocks) and replay-join/--keys-prefix and
  replay-make/--keys drive it.
- IMA ADPCM (mono) sound: the writer encodes raw s16 PCM per chunk, each
  chunk a 4-byte state header then 4-bit codes so the player can start there.
  replay-join/replay-make --sound-encode adpcm is the built-in SoundA4
  (format 1, 4 bits); adpcm2 is the named-decompressor form ("2 adpcm");
  both emit the same bytes. Round-trips a tone at ~32 dB.
- A one-shot `tools/replay-make` driver: ffmpeg -> replay-encode -> replay-join
  in a single command (aspect-correct height, type 19 video, VIDC-E8 audio, a
  first-frame or supplied poster), cleaning up its intermediate files.
- A signed-16-PCM sound encoder (`replay_sound`): 8-bit VIDC exponential as the
  nearest-match inverse of Acorn's exact `ELogToLinTable` (verified ~37 dB
  round-trip SNR on real audio), plus 8-bit and 16-bit signed linear. The
  bits-per-sample label selects the player's format-1 decoder (SoundE8/S8/S16).
  `replay-join` exposes it as `--sound-pcm FILE --sound-encode vidc-e8|signed-8|
  signed-16`, taking canonical `s16le` produced by ffmpeg.
- A shared `mb_frame_verify` dispatcher for the HQ-derived block grammar used
  by types 17 and 19: stationary, temporal, spatial, split-child ordering,
  current-frame reference legality, scan completion, and strict zero padding.
  Codec callbacks retain each format's distinct data-block reconstruction.

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
- `LionFishSMB,ae7`, a separately generated type 19 (Super Moving Blocks)
  encode, has video and audio payloads byte-identical to `LionFish19,ae7`.
- On the same 25 authoritative type 2 source frames at quality 7, the portable
  encoder emits 181,220 bytes versus Acorn's 181,885 bytes. Portable luma PSNR
  is 42.765507 dB versus Acorn's 45.221729 dB; both have maximum luma error 2.
- Spatial references are checked against reconstruction order. In particular,
  a split child cannot read pixels from a future top-level 4x4 parent.
- Normal and ASan/UBSan test suites cover the C implementation; Unicorn tests
  run when its Python bindings and the compiled decoder are available.
- Type 17 data-coded 4x4 and split data-coded 2x2 reconstruction agree
  byte-for-byte with compiled Decomp17 in native YUV555. The split fixture
  gives each quadrant distinct chroma, so it also verifies 2x2 child order and
  raster placement rather than only decoded sample values.
- Compiled Decomp17 also agrees on stationary and temporal/spatial 4x4 copies,
  plus a mixed split containing stationary, temporal, and same-parent spatial
  2x2 children. The harness's explicit `--previous-layout yuv555` keeps these
  native words distinct from type 19's default `6y5uv` previous-frame layout.

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
- Source-matched measurement shows the current ordered policy is 0.366%
  smaller than Acorn on the first comparison chunk but 2.456 dB worse in luma
  PSNR. It selects many more stationary and far fewer spatial blocks.
- The cross-family lowest-error policy is the default portable approach.
- On the same chunk, lowest-error emits 179,656 bytes, 1.225% below Acorn,
  with 45.236548 dB luma PSNR versus Acorn's 45.221729 dB. U and V PSNR remain
  0.240 dB and 0.462 dB below Acorn respectively.
- A five-point fixed-level sweep shows lowest-error is smaller and higher
  quality through level 14. At levels 21 and 28 it spends more bytes to retain
  about 0.9 dB additional luma PSNR, motivating matched-target measurement.
- At a 6,000-byte target over the 25-frame corpus, lowest-error emits 149,184
  bytes at 40.663672 dB luma versus ordered's 150,004 bytes at 38.965991 dB.
- `LionFishX,ae7` extracts as exactly 375 frames. The first 25 extracted frames
  match the previously validated comparison corpus byte-for-byte.

## Known Gaps

- CompLib RGB conversion constants are source-derived but not yet compared
  byte-for-byte with an ARM conversion fixture.
- Acorn's chunk-budget carry and three-level `Cut` escape are not implemented;
  they require real Replay container/chunk accounting.
- The AE7/Replay container writer is implemented and round-trips through the
  reader and the compiled Decomp19, but actual RISC OS Player playback is not
  verified here (no player in this environment). The writer takes already
  encoded sound bytes and emits the sound-format field, including the
  `2 <name>` form for the extensible named decompressors (adpcm, GSM, G72x,
  MPEG). No audio *encoder* is implemented, and the automatic per-chunk time
  slicing is only correct for format 1 (8-bit VIDC) and uncompressed linear PCM;
  framed format-2 codecs need explicit per-chunk byte boundaries. The key-frame
  area is plumbed through
  (`write_keys`) but no type 19 key-blob generator exists yet, so movies are
  written with `-1 (no keys)` by default.
- Temporal candidate measurements are cached in an explicit per-frame
  workspace across target-byte retries. One motion scan records the best
  vector for all 29 quality rows. Spatial candidates remain live because they
  depend on the quality-specific partial reconstruction of the current frame.
- On the five-frame, 6,000-byte target regression, caching leaves all payloads
  byte-identical and reduces measured temporal pixel comparisons from
  7,067,778 to 3,267,426 (53.8%). Fixed-level CLI runs do not enable the cache,
  avoiding its all-quality-row setup cost where no retry can reuse it.
- Type 2 extraction requires `--type2-layout type19-fields`. It interprets a
  stored halfword as `Y[5:0], U[10:6], V[15:11]` to reproduce the historical
  type 19 compressor input; it is not a general RGB555/YUV555/6Y5UV converter.
- Type 23, 6Y6Y5U5V, is packed 4:2:2 rather than full per-pixel YUV: two
  horizontal six-bit luma samples share one five-bit U/V pair, with no vertical
  chroma subsampling. Types 8 and 21 may also map usefully to FFmpeg RGB24,
  YUV24, or YUYV pipelines after their exact packing is verified.
- Selecting the historical compressor colour-space label `6YVUV` has been
  observed to produce an unplayable movie. The cause is unconfirmed, so new
  layout conversions must use explicit names and player-validation fixtures.
- The confirmed type 7-to-type 2 source path exposes a CompLib limitation:
  `-Convert 6Y5UV` changes the type 2 label but does not convert type 7 YUV555
  words because Decomp7 has no `Dec24`. Comparison tooling must preserve this
  historical word interpretation when reproducing the Acorn encode.
- `LionFishX,ae7` is the validated type 2 YUV intermediate: 16,118,852 bytes,
  SHA-256 `f6a71e4e73dda589d131146ae0de79f4e350fbdcd2fe7bed891e3a39b1b41020`.
- Type 17 now has a complete portable verifier but no encoder. Types 7 and 20
  still have only descriptors and notes. Moving Lines remains separate future
  work.
