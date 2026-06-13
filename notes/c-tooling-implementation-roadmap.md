# C Tooling Implementation Roadmap

This note turns the Moving Blocks implementation plan into a staged roadmap for
portable C tooling. The immediate goal is a self-documenting encoder that can
read modern video through FFmpeg's processing pipeline and emit Acorn Replay
video, starting with format 19, `Super Moving Blocks`.

## Decisions So Far

- Write the codec core in C.
- Keep the codec core independent from input decoding and Replay container
  writing.
- Implement Moving Blocks format 19 first.
- Read raw `rgb24` frames from stdin or a file first, normally produced by an
  external `ffmpeg` command.
- Add optional libav/FFmpeg library input after the compressor and verifier are
  stable.
- Build verifier and trace tooling early, before chasing compression ratio.
- Park the RO3.71/RO2003 comparison unless a behaviour mismatch needs it.
- Park H.263 because it is an industry-standard codec with separate
  documentation.

The first implementation does not need to reproduce every Acorn compressor
decision. It must produce legal streams, keep encoder reconstruction in sync
with the decoder, and explain why it chose each block mode.

## High-Level Architecture

Use one-way dependencies:

```text
frame source -> frame normaliser -> codec encoder -> Replay writer
                                      |
                                      +-> verifier
                                      +-> trace output
```

The codec encoder receives normalised frames. It should not know whether they
came from raw stdin, a test fixture, image files, or future libav integration.

The Replay writer receives compressed frame payloads. It should not know how
Moving Blocks chooses stationary, temporal, spatial, or data-coded blocks.

This gives useful intermediate tools:

- payload-only encoder for early codec work;
- payload verifier for decoder-state checks;
- reconstructed-frame dumper for visual inspection;
- final Replay writer once payloads are trusted.

## Proposed Repository Layout

The exact names can change, but the code should stay split by responsibility:

```text
src/
  replay_buffer.[ch]          growable byte buffers
  replay_bitwriter.[ch]       LSB-first bitstream writer
  replay_halfword.[ch]        halfword stream helpers for Moving Lines later
  replay_frame.[ch]           frame allocation, strides, pixel access
  replay_color.[ch]           RGB to Replay working formats
  replay_input_raw.[ch]       raw stdin/file frame input
  replay_packet.[ch]          compressed frame payload object
  replay_container.[ch]       Replay/AE7 movie writer

  mb/
    replay_mb_codec.[ch]      Moving Blocks descriptors and shared state
    replay_mb_tables.[ch]     Huffman, motion, and threshold tables
    replay_mb_predict.[ch]    luma prediction and chroma helpers
    replay_mb_match.[ch]      temporal/spatial candidate generation
    replay_mb_encode.[ch]     frame/block loop and candidate selection
    replay_mb_decode_ref.[ch] verifier decoder for generated payloads
    replay_mb_trace.[ch]      block decision trace output
    mb7_codec.[ch]            format 7 rules, later
    mb17_codec.[ch]           format 17 rules, later
    mb19_codec.[ch]           format 19 rules, first
    mb20_codec.[ch]           format 20 rules, later

  ml/
    replay_ml_decode_ref.[ch] Moving Lines verifier, later
    replay_ml_encode.[ch]     Moving Lines encoder, later

  tools/
    replay-enc.c              encoder CLI
    replay-verify.c           payload/container verifier CLI
```

Use CMake for host-tool builds, tests, and optional FFmpeg library discovery.
Keep the codec core plain C with no mandatory third-party dependencies.

## Shared Data Model

The first code should prefer clarity over packed memory density:

```c
typedef struct {
    uint8_t y;
    uint8_t u;
    uint8_t v;
} ReplayPixel;

typedef struct {
    int width;
    int height;
    int stride;
    ReplayPixel *pixels;
} ReplayFrame;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ReplayPacket;
```

For format 19, `ReplayPixel` values are quantised to `6Y5UV` before encoding:
6-bit Y, 5-bit U, and 5-bit V. Other formats can reuse the same struct while
changing the valid precision through a codec descriptor.

## Stage 0: Project Skeleton

Create the build and test harness first:

- CMake project with strict warnings enabled;
- small test executable for unit tests;
- CLI skeletons for `replay-enc` and `replay-verify`;
- common error-reporting helpers;
- no FFmpeg library dependency yet.

The first test targets should build on a normal Unix-like host without RISC OS
tools or FFmpeg development headers.

## Stage 1: Core Writers

Implement:

- growable byte buffers;
- LSB-first bit writer for Moving Blocks;
- little-endian halfword writer/reader for Moving Lines tests later;
- packet reset and ownership helpers.

Tests:

- known bit patterns crossing byte boundaries;
- flush behaviour for partially filled bytes;
- no writes past buffer capacity;
- deterministic reset and reuse.

This stage is small but important. A wrong bit-order assumption will make every
later codec test misleading.

## Stage 2: Format-19 Data-Only Payloads

Implement enough format-19 support to encode every 4x4 block as literal data:

- format-19 descriptor;
- 64-symbol luma residual Huffman table;
- luma prediction and reconstruction;
- literal 5-bit U/V writing;
- 4x4 data-coded block writer;
- 2x2 data-coded block writer, initially optional;
- reference decoder for those same block forms.

Tests:

- every Huffman residual symbol round-trips through the verifier;
- one 4x4 block round-trips exactly in `6Y5UV`;
- one 2x2 block round-trips exactly in `6Y5UV`;
- synthetic full frames round-trip with data-coded blocks only.

This creates a large but valid codec payload path before any motion search or
rate control is attempted.

## Stage 3: Raw FFmpeg Pipe Input

Add raw frame input:

- `--input -` for stdin;
- `--input-format rgb24`;
- `--size WIDTHxHEIGHT`;
- `--fps NUM/DEN` or decimal input normalised internally;
- RGB to `6Y5UV` conversion;
- optional reconstructed PPM dump for visual checks.

First usable development command:

```text
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 -pix_fmt rgb24 -f rawvideo - \
  | replay-enc --codec 19 --input - --input-format rgb24 \
      --size 320x256 --fps 25/2 --data-only \
      --payload-out frames.mb19 --dump-recon recon/
```

This depends only on the `ffmpeg` executable. The encoder still has no libav
build dependency.

## Stage 4: Basic Compression Modes

Add block candidates in increasing order of state complexity:

1. stationary copy from the same position in the previous decoded frame;
2. temporal copy from nearby previous-frame blocks;
3. spatial copy from already reconstructed current-frame pixels;
4. split 2x2 decisions versus a single 4x4 decision.

For each candidate, compute:

- legal bit cost;
- reconstructed pixels;
- component and total error;
- whether it passes the current loss thresholds.

The encoder must write a block and immediately reconstruct it into the current
frame. Later candidates must read reconstructed pixels, not original source
pixels.

## Stage 5: Rate Control And Trace

Recreate the Acorn-style frame retry loop in a clear form:

- start with a configured `loss_level`;
- encode the whole frame;
- if the output is too large, increase loss;
- if the output is too small, decrease loss;
- retry from a clean per-frame state snapshot.

Use exact bit costs once available. Add trace output beside candidate selection,
not inside low-level bit-writing code.

Useful trace lines:

```text
frame=12 retry=1 loss=8 bytes=3476 target=3200..3600 modes=stat:83,temp:211,spat:19,data4:24,split2:7
frame=12 block=40,28 mode=temporal dx=0 dy=-1 bits=6 error=18 threshold=44
frame=12 block=44,28 mode=data4x4 bits=71 error=0 ypred_in=17 ypred_out=19
frame=12 block=48,28 mode=split2x2 bits=64 error=9 reason=cheaper_than_4x4
```

Development switches worth adding:

- `--data-only`;
- `--no-spatial`;
- `--no-motion`;
- `--block-trace X,Y`;
- `--dump-recon DIR`;
- `--trace FILE`.

## Stage 6: Minimal Replay Container

Only after payload verification is reliable, add a video-only Replay/AE7
writer:

- compression type 19;
- movie width and height;
- frame rate;
- frame chunks;
- key-frame handling;
- payload sizes and offsets.

The CompLib notes and Replay documents should guide this, but the C tool does
not need to reproduce CompLib's structure. CompLib is most useful for policies:
chunk sizing, key-frame cadence, frame retries, and command-line option
meaning.

At the end of this stage, the target workflow becomes:

```text
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 -pix_fmt rgb24 -f rawvideo - \
  | replay-enc --codec 19 --input - --input-format rgb24 \
      --size 320x256 --fps 25/2 --output movie,replay \
      --quality 8 --trace decisions.txt
```

Internally, prefer the name `loss_level`; accept `--quality` as a compatibility
or user-facing alias if useful.

## Stage 7: More Formats

Add other Moving Blocks formats after format 19 is playable:

- format 17: similar HQ block structure with `5Y5UV` and a 32-symbol luma
  Huffman table;
- format 20: similar to format 19 but with `6Y6UV` and stateful delta-coded
  chroma through `D4tab`;
- format 7: older Moving Blocks format, useful as a compatibility target but
  less representative of the later compressors.

The format descriptor boundary should make these additions local to tables,
pixel precision, predictor state, and block-writing rules.

## Stage 8: Moving Lines

Moving Lines should be a separate codec core, sharing only outer tooling:

- frame sources;
- colour conversion policies;
- packet buffers;
- Replay container writing;
- verifier and trace conventions.

Implement the Moving Lines verifier before its compressor:

- literal 15-bit pixels;
- temporal/spatial copy commands;
- same-position previous-frame copy command;
- packed literal runs;
- repeated-pixel runs;
- end-of-frame marker.

Then implement the horizontal run matcher. This codec is useful for graphics
and repeated scanline content, but it is not the best first target for modern
video input.

## Stage 9: Optional libav Input

Once the raw-pipe workflow is stable, add an optional frame source backed by
FFmpeg libraries:

- `libavformat` for demuxing;
- `libavcodec` for decoding;
- `libswscale` for scaling and pixel conversion;
- possibly `libavfilter` later for full filter graphs.

Keep it compile-time optional. The codec API should receive the same
normalised frames whether the source is raw stdin or libav.

The standalone command can then become:

```text
replay-enc --input input.mp4 --size 320x256 --fps 25/2 \
  --codec 19 --output movie,replay --quality 8
```

## Milestones

M1: bit writer and format-19 Huffman tests pass.

M2: `replay-verify --codec 19` decodes generated data-only payloads.

M3: `replay-enc --codec 19 --data-only` reads raw `rgb24` frames and produces
verifier-clean payloads plus reconstructed-frame dumps.

M4: stationary, temporal, spatial, and split-block modes reduce payload size
while verifier output still matches encoder reconstruction.

M5: trace output explains frame retries and block decisions.

M6: video-only Replay file plays through an existing Replay player or
decompressor path.

M7: optional libav input supports direct modern video files without changing
the codec core.

## Remaining Implementation Validation

- Determine the smallest video-only Replay/AE7 container accepted by existing
  players through generated-file tests.
- Use the local verifier for every frame, then use the original decompressor in
  a RISC OS emulator as the compatibility oracle when payload generation is
  available.
- Use CompLib-compatible RGB-to-YUV conversion by default.
- Expose `--loss-level` and `--target-bytes`; retain `--quality` only as an
  optional compatibility alias.
