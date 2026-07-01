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
- Player-compatibility constraints learned from the player source and emulator
  testing (see `docs/player-bugs.md` for the bugs, code locations and player
  version): an audio movie must have at least two chunks (a one-chunk
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
- Encoders for all three Moving Blocks codecs. The shared frame encoder
  (`mb_encode_frame`, with the grammar-independent copy-family selectors
  `mb_encode_select_copy4x4/2x2`) drives type 17 (Moving Blocks HQ) and type 19
  (Super Moving Blocks) as thin adapters. Type 7 (Moving Blocks) has its own
  frame encoder (`codec_movingblocks_encode_frame`) because its grammar differs
  (`1` data / `00`/`0` move / `01` split, no distinct stationary opcode, +/-4
  motion, literal data); it reuses the shared search and copy helpers. All are
  exposed through `replay-encode --codec 7|17|19` and `replay-make --codec`.
- A complete type 7 pipeline: literal-data decoder/verifier, the +/-4 motion
  module (`mb_motion_read_format7`/`write_format7`, format7 vector enumerators),
  and the encoder above. Type 7's spatial tables are identical to 17/19's
  (`Docs/Stream` scrambles the 2x2 column; the running Decomp7 is the authority).
- A standing full-movie cross-check gate (`test_fullmovie_decomp7/17/19`): a
  multi-frame synthetic scene encoded by the real encoder, decoded frame by
  frame on the genuine compiled Decomp module and compared byte-for-byte against
  the encoder's reconstruction. Bands of the scene force each copy family, and
  the generator asserts the encoder exercised them so the gate cannot silently
  weaken. This is the coverage the focused fixtures lacked.

- MovieFS (Warm Silence Software) re-encapsulated PC codecs (video formats
  600–699) in `replay-transcode`. The transcoder strips MovieFS's 16-byte
  per-frame wrapper (`src/replay_moviefs.c`, `replay_moviefs_unwrap_chunk`) and
  drives the codec's colour-table-free `Dec24` variant in 32-bit ARM mode — its
  `FNplook` is empty (or a patchable pass-through), so it does the full YUV→RGB
  conversion with internal tables and never needs the caller-supplied colour
  table the screen-painting `Decompress` variants require (those infinite-loop
  unpatched). `Dec24` emits 24bpp RGB (`COL_RGB888`). Wired: 602 Cinepak
  (validated byte-for-byte against ffmpeg on a real 160×120 movie), plus 608/626
  RGB24 and 615 QT-RLE24 (same harness-compatible variant, pending sample
  validation). See `docs/moviefs-nut-passthrough.md`.
- Codec pass-through to NUT (`--output-format nut`) for codecs the sandbox can't
  run: instead of decoding, each de-wrapped codec frame is muxed into the NUT
  video stream under a codec fourcc (`passthrough_video`, `ReplayFrameWrapIter`),
  letting ffmpeg decode. Wired for Indeo — 628/629 (MovieFS `IV31`/`IV32`) and
  the IMS VideoFS codecs 901 (raw YVU9 → `YVU9`/rawvideo) and 902 (Indeo 3.2 →
  `IV32`). VideoFS uses the same 16-byte wrapper as MovieFS but a different size
  field (`data_len + 28` vs `+ 12`), reverse-engineered from the Decomp901/902 C
  sources. The pass-through machinery is validated by routing Cinepak through it;
  the Indeo mappings are not yet validated against a real movie (none available).
  9xx are decode-only (no compressor exists).
- MovieFS palettised codecs via the `Dec8` variant (600 CRAM8, 604 SMC, 606/624
  RGB8, 607 RLE8, 609 QT-RLE8, 613 QT-RLE4). `Dec8` is r3-free and emits packed
  8-bit palette indices; `convert_frame` gained a packed-byte `COL_PAL8` path
  (`packed8`). **600 CRAM8 is validated end to end** (Big_Ship, Explosions): its
  chunks aren't laid out like the others -- each begins with a `0xffffffff` marker
  and an inline 256-entry RGB555 palette, then the normal wrapper chain. The
  transcoder extracts that per-chunk palette (`moviefs_palette`), and Dec8 writes
  rows bottom-up (`bottom_up`, AVI origin), so `convert_frame` flips. The other
  Dec8-family mappings remain source-derived/unvalidated. Also: **MovieFS carries
  the true frame size in the per-frame wrapper** -- the AE7 header rounds the width
  up (CRAM8/Cinepak `160`→`156`), so the transcoder peeks the first wrapper and
  decodes/presents at the true size (`moviefs_true_size`). DL (622) and ANM
  (623) join the family (their `Dec8` takes no palette); FLIC (610) is excluded
  (per-frame in-stream palette in its workspace). 614 QT-RLE16 is wired via its
  r3-free `Decompress` (16bpp RGB555). Pass-through (NUT→ffmpeg) covers 601
  CRAM16, 603 RPZA, 605 Ultimotion and 610 FLI/FLC in addition to the Indeo
  codecs (610 FLIC's decode path validated with a synthesised FLI_FRAME: ffmpeg's
  flic reads the in-stream FLI_COLOR palette with no extradata). The WSS
  freeware decoder modules used by the native paths are vendored under
  `vendor/armovie-codecs` (the specific `Dec24`/`Dec8`/`Decompress` variant +
  `Info` per codec), so the default `--modules-dir` drives them.
- Eidos "Escape 2.0", Replay type **130**, decoded natively by
  `src/replay_escape130.c` (`direct_esc130` dispatch). A YCbCr 2×2-block codec: a
  persistent, delta/skip-coded grid of 2×2-pixel blocks (base luma + chroma pair +
  optional per-sub-pixel texture) that the display 2× upsamples with a separable
  blend. The bitstream decoder is a clean-room implementation from the format spec;
  the render (`src/dec130_render.c`) is a hand-written reimplementation of
  `DEC130.DLL`'s display path, bit-exact to the DLL. This replaced an earlier NUT
  pass-through to ffmpeg's `escape130` (whose render is not the DLL's). Validated
  byte-identical to a standalone reference decoder on all seven `ESCAPE 2.0` samples
  and other games' 130 movies. See `docs/spec/eidos-escape.md`.
- Eidos "Escape 122", Replay type **122**, decoded natively by
  `src/replay_escape122.c` (`direct_esc122` dispatch). Despite the "Escape" name it
  is a *palettised* (PAL8) codec, unrelated to 124/130 -- the proprietary Streamer
  DLLs can't decode it; only the Eidos DOS player did. 8×8 superblocks of 2×2
  macroblocks, inline per-chunk VGA palette (6→8-bit), delta-coded. Clean-room
  implementation from the format spec (no existing decoder consulted), validated
  byte-for-byte against a standalone reference on `tank.rpl` (320×200, 1925 frames,
  video + sound). See `docs/spec/eidos-escape.md`.
- Eidos "Escape 124", Replay type **124**, decoded natively by
  `src/replay_escape124.c` (`direct_esc124` dispatch). A games-era RGB555 block
  codec (`WINSDEC`/`EDEC`): 8×8 superblocks of 2×2 macroblocks, three rotating
  codebooks, an escalating skip VLC, and mask/pattern placement passes. A single
  video chunk concatenates the header's "frames per chunk" frames, each
  `[flags][size][bitstream]`; the transcoder walks them by `size`. Reimplemented
  from the WINSDEC reverse-engineering (cross-referenced with FFmpeg's `escape124`
  algorithm); validated byte-for-byte against both a standalone reference decoder
  and FFmpeg's own `escape124` on `ESCAPE.RPL` (320×240) and `PYRAMID.RPL`
  (320×120). Escape 100/102 (ARM modules) are not yet wired. See
  `docs/spec/eidos-escape.md`.
- Eidos "Escape"/WINSTR **sound format 101** (the 4-bit ADPCM these movies use),
  decoded by `src/replay_escape_adpcm.c` and muxed -- so type-130 movies transcode
  with sound. It is a non-canonical IMA ADPCM (no `step>>3` bias, two altered
  step-table entries), high-nibble-first, with one running state for the whole
  movie (no per-chunk reset/header). Clean-room reconstruction from WINSTR.DLL;
  validated bit-exact on `Victory`/`inflight`. See `docs/spec/armovie-sound.md` §4.
- LinePack (Henrik Bjerregaard Pedersen, 1995), Replay type **800**, via its
  vendored `Decomp800` module under the CodecIf harness. The codec decodes at the
  *exact* declared size (its Info "step" is an alignment hint, not frame padding),
  so `codec_info` marks it `exact_size` and `transcode_video` skips block rounding
  for it — block-rounding 160×120→160×128 over-fills each frame and desyncs the
  source. Validated end to end on `TEKTRAILER.rpl` (475 frames, correct images).
  See `docs/spec/linepack-type800.md`.
- Iota "The Complete Animator" (TCA / ACEF), Replay type 500, decoded natively by
  `src/replay_tca.c` (`replay_tca_open`/`replay_tca_next_frame`) and driven by
  `replay-transcode`'s `direct_tca` dispatch (`codec_info` case 500). It walks the
  embedded IotaFilm (ACEF film header + PALE palette), decodes the Euclid frame
  blocks (variable-width LZW / RLE / raw, with Delta XOR) for the 8-bit screen
  modes (28/21), and emits 8bpp indices + palette through `COL_PAL8` → RGB24.
  All numbered screen modes are handled (8-bit 28/21/15/36/40, 4-bit 27/12/13/39),
  plus new-format RISC OS sprite mode words: the colour depth is the sprite type
  in bits 27-30 (4bpp→mode 27, 8bpp→mode 28), and 16bpp is a direct-colour path
  emitting packed RGB555 → RGB24 (validated on the 240x180 `080050` film). The
  Iota soundtrack (`SOUN` WAV1 8-bit VIDC-log / WAV2 4-bit ADPCM) is decoded to
  mono PCM by `replay_tca_decode_audio` and muxed via a new `AUDIO_IOTA` sound
  format, so type-500 movies transcode with sound. `test_replay_tca` covers the
  raw/LZW/Delta, 4-bit-mode and WAV1/WAV2 audio paths; the real BUCCAN movie
  transcodes end to end with video + narration. Remaining: multi-chunk handling
  (issue #34) and audio rate/sync refinement. See `docs/spec/tca-type500.md`.

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
- The type 7 encoder's output decodes byte-for-byte on compiled Decomp7: hand-
  laid spatial2x2 fixtures plus encoder-produced key and inter frames, and the
  full-movie gate. A real 1892-frame movie (`replay-make --codec 7`) plays with
  correct colour on the RISC OS Replay player. Finding this required fixing a
  scrambled type 7 2x2 spatial table that the encoder and verifier shared (so
  self-checks passed) but real Decomp7 did not; see the full-movie gate.
- Every type 17 and type 19 table entry was probed one-by-one against the
  compiled Decomp17/19: 288 temporal vectors, 8 4x4-spatial, 8 2x2-spatial, and
  the luma Huffman table (32 symbols for type 17, 64 for type 19) -- zero
  mismatches in either codec. All three Moving Blocks codecs are now
  table-complete-verified against their real Decomp modules.

## Deliberate Policy Choices

- The codec core performs one deterministic pass. Target-size retry policy is
  in shared `mb_rate_control` code.
- Top-level copy-mode priority is stationary, temporal, then spatial. Temporal
  and spatial searches retain the lowest accepted error, with table order as
  the tie-break. This is compatible stream generation; Acorn decision parity
  is not required, but its bitrate and quality effects must be measured and
  documented.
- A 4x4 data candidate wins a bit-cost tie with a split candidate.
- The lowest-error policy's copy bit-cost tie-break uses `mb_encode_motion_bits`,
  which models the 17/19 motion coding. The type 7 encoder reuses it as a proxy:
  its +/-4 codes differ slightly (e.g. stationary is 4 bits, not the proxy's 7),
  so an equal-error tie may pick a marginally larger type 7 copy. It never
  affects correctness or the reported/actual size, only rare tie ordering.
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

- Final-chunk padding. When the last chunk is partial (e.g. 63 frames at 25
  frames/chunk gives chunks of 25, 25, 13), the RISC OS player shows a black
  screen with an increasing number of random error blocks past the real frames.
  Acorn's compressor appears to pad the final chunk by repeating the last frame
  ("repeating last frame") so every chunk holds frames-per-chunk valid frames;
  replay-make/replay-join emit a short final chunk, so the player decodes beyond
  the encoded frames and accumulates inter-frame errors. Likely fix: pad the
  last chunk with stationary/repeat-last frames. Observed on a type 20 clip; not
  yet confirmed whether full-length 7/17/19 movies are affected.
- The RGB->YUV conversion is verified against CompLib's ARM source (Tools/
  CompLib RGB->YUV routine). The *algorithm* matches bit-exact: an independent
  reimplementation from that assembly equals mb_color over an RGB sweep for both
  6Y5UV and YUV555, undithered (`test_mb_color_complib`). CompLib dithers with
  Floyd-Steinberg error diffusion; mb_color uses ordered/no dither by design, so
  dithered outputs differ deliberately and are not compared. The seven
  coefficients are CompLib's `DCD 65536*<c>` words; all seven equal the round-to-
  nearest of those expressions, confirmed against BBC BASIC's exact float values
  (38469.632->38470, 22116.551->22117, 10657.780->10658, etc.). This closes the
  former "RGB conversion constants unvalidated" gap.
- Acorn's chunk-budget carry and three-level `Cut` escape are not implemented;
  they require real Replay container/chunk accounting.
- The AE7/Replay container writer round-trips through the reader and the
  compiled Decomp modules, and complete movies (types 19, 17 and 7, with audio
  and a poster) have been played on the RISC OS Replay player and !ARPlayer with
  correct colour. Player testing is manual on an emulator; it is not automated in
  this environment. The writer takes already encoded sound bytes and emits the
  sound-format field, including the `2 <name>` form for the extensible named
  decompressors (adpcm, GSM, G72x, MPEG). The automatic per-chunk time slicing is
  only correct for format 1 (8-bit VIDC), IMA ADPCM and uncompressed linear PCM;
  other framed format-2 codecs need explicit per-chunk byte boundaries. Per-chunk
  key frames are generated for type 17/19 (`replay-encode --keys-prefix`,
  `replay-join`/`replay-make --keys`); the default remains `-1 (no keys)`.
  Whether the player actually *seeks* using them is not yet acceptance-tested.
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
- Types 7, 17 and 19 each have a complete portable verifier and encoder, all
  cross-checked against their compiled Decomp modules. Type 20 (Moving Blocks
  Beta, 6Y6UV) still has only a descriptor and notes. Moving Lines remains
  separate future work.
- Remaining type 7 polish: the motion bit-cost proxy noted under Deliberate
  Policy Choices (tie-break only). No correctness impact.
- The RGB->YUV conversion constants remain the one input-chain link not yet
  cross-checked against an ARM conversion fixture (see first gap).
