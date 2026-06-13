# Moving Blocks C Implementation Plan

This note describes a practical implementation plan for a portable,
self-documenting Moving Blocks compressor. The recommended first target is
Replay format 19, `Super Moving Blocks`.

The goal of the first implementation is not maximum compression speed or best
modern rate/distortion performance. The goal is a readable encoder whose output
can be explained, decoded, and compared against the original Acorn codec.

## Language Choice

C is the better first language for this project.

Reasons:

- the original codecs are low-level ARM/BASIC code, so their data flow maps
  cleanly to C structs, arrays, and explicit bit operations;
- a C implementation is easier to keep portable to RISC OS, Unix-like systems,
  and small test harnesses;
- the bitstream format is fixed, so object abstractions are less important than
  exact control over layout, state, and side effects;
- a simple C API is easy to call from C++, Python bindings, command-line tools,
  or future GUI wrappers.

C++ could be useful later for tooling around the codec: batch pipelines, image
I/O wrappers, experiments, profiling front-ends, or richer test frameworks. It
is not necessary for the core encoder, and it may make the first implementation
less transparent if templates, ownership wrappers, or class hierarchies obscure
the codec state.

Recommended compromise: write the codec core in plain C, with headers that are
also C++-compatible:

```c
#ifdef __cplusplus
extern "C" {
#endif

/* public API */

#ifdef __cplusplus
}
#endif
```

## Multi-Format Shape

Format 19 should be the first implemented codec, but the code should not be
named or structured as if format 19 is the only possible output.

Use shared Moving Blocks machinery where the formats genuinely overlap, and
put bitstream differences behind a small format descriptor.

Shared concepts:

- frame buffers and pixel conversion;
- least-significant-bit-first bit writing;
- 4x4 scan order;
- reconstructed-frame feedback;
- temporal and spatial candidate search;
- quality/loss threshold tables;
- candidate scoring and tracing;
- frame-level retry control.

Per-format concepts:

- working pixel precision: `5Y5UV`, `6Y5UV`, or `6Y6UV`;
- top-level block opcodes;
- 2x2 opcodes;
- motion/spatial code tables;
- data-coded block layout;
- luma predictor precision;
- Huffman residual table;
- chroma coding: literal U/V for formats 7, 17, and 19; delta-coded state for
  format 20.

The public encoder can expose a format id:

```c
typedef enum {
    REPLAY_CODEC_MOVING_BLOCKS_7  = 7,
    REPLAY_CODEC_MOVING_BLOCKS_HQ = 17,
    REPLAY_CODEC_SUPER_BLOCKS     = 19,
    REPLAY_CODEC_BLOCKS_BETA      = 20
} ReplayCodecId;
```

Internally, each codec can provide a descriptor:

```c
typedef struct ReplayMbCodec ReplayMbCodec;

struct ReplayMbCodec {
    ReplayCodecId id;
    const char *name;
    ReplayPixelFormat working_format;

    int block_width;
    int block_height;
    int motion_radius_x;
    int motion_radius_y;

    const ReplayHuffmanTable *luma_huffman;
    const ReplayMotionTable *motion4x4;
    const ReplayMotionTable *motion2x2;

    void (*reset_frame_state)(ReplayMbState *state);
    int  (*write_data_4x4)(ReplayMbEncoder *enc, int x, int y);
    int  (*write_data_2x2)(ReplayMbEncoder *enc, int x, int y);
};
```

The decision-complete version of this boundary is in
`notes/c-tooling-interface-specification.md`. The important point is that the
block search code asks the selected codec how expensive and how legal a
candidate is, rather than hard-coding format-19 bit patterns throughout the
encoder.

## Encoder Layers

Keep the implementation split into layers with one-way dependencies:

```text
input reader -> frame normaliser -> codec encoder -> Replay writer
                                      |
                                      +-> verifier/trace
```

The codec encoder should not know whether frames came from FFmpeg, raw stdin,
test images, or a future RISC OS source reader. It should receive already-sized
`ReplayFrame` objects in a declared input pixel format.

The Replay writer should not know how Moving Blocks chooses modes. It should
receive compressed frame payloads and write the surrounding Replay/AE7 chunk
structure, key-frame metadata, and sound interleave later if needed.

This separation gives three useful test points:

- pixel conversion tests before compression;
- raw codec payload tests before AE7 container work;
- container tests once payload generation is trusted.

## Public API Sketch

The first API should be small enough to use from a command-line tool and from
tests:

```c
typedef struct ReplayEncoder ReplayEncoder;

typedef struct {
    ReplayCodecId codec_id;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int target_bytes_per_frame;
    int loss_level;
    unsigned flags;
} ReplayEncoderConfig;

typedef struct {
    ReplayInputPixelFormat format;
    const void *data;
    size_t stride;
} ReplayInputFrame;

int replay_encoder_create(const ReplayEncoderConfig *config,
                          ReplayEncoder **out_encoder);

int replay_encoder_encode_frame(ReplayEncoder *encoder,
                                const ReplayInputFrame *source,
                                ReplayPacket *out_packet);

void replay_encoder_destroy(ReplayEncoder *encoder);
```

`ReplayPacket` can initially be just a growable byte buffer containing one
compressed video frame. It can later grow timestamp, key-frame, and chunk
metadata without changing the codec internals.

## Pixel Pipeline

The codec core should use a normalised internal pixel representation even if
the final bitstream uses packed values.

Suggested internal representation:

```c
typedef enum {
    REPLAY_PIX_RGB24,
    REPLAY_PIX_RGBA32,
    REPLAY_PIX_GRAY8,
    REPLAY_PIX_YUV555,
    REPLAY_PIX_6Y5UV,
    REPLAY_PIX_6Y6UV
} ReplayInputPixelFormat;

typedef struct {
    uint8_t y;
    uint8_t u;
    uint8_t v;
} ReplayPixel;
```

For format 19:

- input `rgb24` is converted to full-range or Replay-compatible YUV;
- Y is quantised to 6 bits;
- U and V are quantised to 5 bits;
- data-coded blocks reconstruct exactly those quantised values.

The colour conversion should be isolated and heavily tested. A wrong colour
matrix or rounding rule will make the compressor look broken even if the
bitstream is correct.

The v1 compatibility policy is to reproduce CompLib's documented fixed-point
RGB-to-YUV coefficients and Replay quantisation. The conversion remains an
isolated named policy so a modern limited-range alternative can be added later
without affecting codec internals.

## State Model

Each encode attempt for a frame needs its own mutable state:

- previous reconstructed frame;
- current reconstructed frame being built;
- source frame in working format;
- bit writer position;
- luma predictor state;
- format-specific predictor state, such as format 20's `prevu`/`prevv`;
- trace collector, if enabled.

Frame retries must restore this state exactly. The simplest first approach is
to rerun the frame from a clean snapshot rather than trying to undo individual
block decisions.

For format 19, the retry snapshot needs at least:

- previous reconstructed frame, read-only for the attempt;
- empty current reconstructed frame;
- cleared luma predictor;
- output byte position reset to the start of the frame.

For format 20, the snapshot also needs chroma predictor state.

## First Target

Implement format 19 first.

Format 19 has the important later Moving Blocks features:

- 4x4 block scan;
- explicit stationary blocks;
- temporal and spatial copy modes;
- 4x4 or four 2x2 coding decisions;
- 6-bit luma;
- 5-bit literal chroma;
- Huffman-coded luma residuals;
- least-significant-bit-first bit output.

It avoids format 20's `D4tab` and `prevu`/`prevv` chroma predictor state, which
makes it a better first target for a clear encoder.

## Compatibility Goal

The first C encoder should produce streams accepted by the Replay format-19
decompressor. It does not need to reproduce the Acorn compressor's exact
choices for every block.

This matters because the codec format defines what the player can decode, while
the compressor source defines one particular search strategy. A simpler encoder
can still be valid if it writes legal blocks and keeps its reconstructed frame
state identical to the decoder's state.

## Core Algorithm

The encoder is a block matcher with reconstruction feedback.

For each frame:

1. Convert or quantise the source frame to the codec working format, initially
   `6Y5UV`.
2. Clear per-frame sample predictor state.
3. Scan the frame in 4x4 blocks from top left to bottom right.
4. For each 4x4 block, evaluate legal coding candidates.
5. Choose a candidate according to quality and size rules.
6. Write the candidate to the bitstream.
7. Reconstruct the decoded pixels immediately into the current output frame.
8. Use that reconstructed frame for later spatial and temporal references.

The key rule is that the encoder must behave like a decoder as it goes. Future
decisions must use reconstructed pixels, not original source pixels.

## Algorithms In Use

### Fixed Block Partitioning

The codec uses a fixed 4x4 macroblock grid. A 4x4 block can either be coded as
one unit or split into four 2x2 decisions.

This is simple and deterministic. It is also much less flexible than modern
variable block-size motion compensation, but the Replay bitstream cannot express
larger or arbitrary block shapes.

### Temporal Block Matching

Temporal candidates copy pixels from the previous decoded frame. Format 19 uses
an 8-pixel search radius in x and y.

The Acorn compressor searches candidate offsets and accepts a match if
component errors fit the current `QP%` thresholds. It is a threshold-driven
matcher, not a modern rate/distortion optimiser.

Better alternatives for the C encoder:

- keep the exhaustive search first, because it is easiest to validate;
- later add early rejection when accumulated error already exceeds the allowed
  threshold;
- later add a fast prefilter using luma sums or low-resolution block signatures;
- optionally search offsets in likely-good order, such as stationary, nearest
  neighbours, then wider offsets.

### Stationary Blocks

Format 19 has an explicit stationary block code. This means "copy the same
position from the previous decoded frame".

The C encoder should test stationary first. It is cheap, common in static
regions, and easy to reason about.

### Spatial Block Matching

Spatial candidates copy from already-decoded pixels in the current frame. These
references must be above or left of the current block in scan order.

Spatial copies can exploit repeated textures within a frame. They are also
state-sensitive, because the source is the reconstructed current frame, not the
input frame.

Better alternatives:

- implement the exact legal spatial table first;
- keep spatial search separate from temporal search in the code, even if the
  bitstream shares motion tables;
- later cache spatial candidate lists per block position.

### Threshold-Based Match Acceptance

The Acorn compressors use the `QP%` table:

- `maxi`: ordinary per-component error limit;
- `maxe`: exceptional per-component error limit;
- `tot16`: total 4x4 error limit;
- `tot4`: total 2x2 error limit.

Increasing `qual%` makes these limits looser. In this source, higher `qual%`
usually means lower visual quality and smaller output.

For a self-documenting C implementation, name this value something like
`loss_level` internally, even if the command-line compatibility option remains
`quality`.

### Data-Coded Blocks

If no acceptable copy is chosen, the encoder writes data-coded pixels.

For format 19:

- 4x4 data stores one literal 5-bit U, one literal 5-bit V, and 16
  Huffman-coded 6-bit luma residuals;
- 2x2 data stores one literal 5-bit U, one literal 5-bit V, and 4
  Huffman-coded 6-bit luma residuals.

Luma residuals are predicted from previous reconstructed luma values and coded
through the fixed 64-symbol Huffman table.

Better alternatives:

- the bitstream requires the fixed Huffman table, so entropy coding cannot be
  changed for format-19 compatibility;
- the encoder can improve decisions by calculating exact data-coded bit cost
  before choosing 4x4 versus 2x2;
- later experiments could add non-compatible formats with different entropy
  coding, but that should be kept out of the Replay-compatible encoder.

### Chroma Averaging

Format 19 stores one U/V pair per data-coded 4x4 or 2x2 block. The source code
uses averaged chroma for these blocks.

Better alternatives within the same bitstream:

- choose chroma values that minimise reconstructed error after quantisation,
  rather than blindly rounding the arithmetic average;
- weight chroma less than luma when scoring if that better matches perceived
  quality;
- keep the first implementation simple and explicit: average, quantise,
  reconstruct, score.

### 4x4 Versus 2x2 Decision

The Acorn compressor tries four 2x2 decisions and compares them with the exact
data-coded 4x4 cost from `test16%`.

The C encoder should express this as candidate scoring:

```text
candidate = {
    mode,
    bit_cost,
    reconstruction_error,
    reconstructed_pixels
}
```

For the first implementation, choose the lowest bit cost among candidates that
meet the current error threshold. Later, a rate/distortion score can improve
visual quality for the same budget.

### Frame-Level Bitrate Control

The Acorn compressor does not directly allocate bits per block. It repeatedly
encodes the whole frame, changing `qual%` until the output size lands inside a
target window.

This is crude but easy to reproduce:

- if the frame is too small, reduce `qual%` for stricter matching and larger
  output;
- if the frame is too large, increase `qual%` for looser matching and smaller
  output.

Better alternatives:

- binary search the loss level instead of stepping linearly;
- keep the Acorn-style retry loop first because it matches the source and is
  simple to inspect;
- later add per-frame diagnostics showing why each retry changed size.

## Input Video Strategy

Using FFmpeg is practical, and it is probably the right way to make this tool
useful with modern video.

There are two sensible integration levels.

### Raw Pipe First

The first input path should read raw frames from stdin or a file. FFmpeg can do
the heavy lifting externally:

```text
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 \
    -pix_fmt rgb24 -f rawvideo - | replay-enc \
    --input - --input-format rgb24 --size 320x256 --fps 12.5 \
    --codec 19 --output out,replay
```

This is the quickest route to modern video input because FFmpeg handles:

- container demuxing;
- codec decoding;
- scaling;
- frame-rate conversion;
- colour conversion;
- filtering.

The compressor only needs a small raw-frame reader plus conversion from the
chosen incoming format into Replay's working YUV representation.

Recommended first raw input formats:

- `rgb24`: simple, widely supported, easy to inspect;
- `rgba`: useful if image tools produce alpha, though alpha is ignored;
- possibly `gray`: useful for tests.

Avoid making planar YUV input the first path unless there is a strong reason.
Replay's formats are packed, low-bit-depth YUV variants, so RGB input keeps the
initial conversion code explicit and avoids confusing FFmpeg's many YUV layout
variants with Replay's internal representation.

### libav/FFmpeg Libraries Later

Using the FFmpeg libraries is also practical, but it adds build and API
complexity:

- `libavformat` for containers;
- `libavcodec` for decoding;
- `libswscale` for scaling and pixel conversion;
- possibly `libavfilter` if the tool wants a full filter graph.

That gives a nicer standalone command:

```text
replay-enc --input input.mp4 --size 320x256 --fps 12.5 \
    --codec 19 --output out,replay
```

The tradeoff is that the compressor now depends on FFmpeg development headers,
library discovery, API-version handling, and platform-specific packaging.

Recommendation:

1. Implement raw stdin/file input first.
2. Document an FFmpeg command line as the supported frontend.
3. Add a libav input module once the compressor and verifier are stable.

This keeps the codec core independent. The encoder should consume a generic
stream of normalised frames, regardless of whether those frames came from a raw
pipe, libav, image files, or tests.

## Proposed C Modules

Keep the core small and explicit:

- `replay_bitwriter.[ch]`: least-significant-bit-first bit output.
- `replay_frame.[ch]`: frame buffers, pixel format conversion, bounds helpers.
- `replay_input_raw.[ch]`: raw stdin/file frame input.
- `replay_input_ffmpeg.[ch]`: optional libav input module, added later.
- `replay_mb_codec.[ch]`: shared Moving Blocks codec descriptors and state.
- `replay_mb_tables.[ch]`: shared motion tables, Huffman tables, `QP%`
  thresholds.
- `replay_mb_predict.[ch]`: luma prediction, residual reconstruction, chroma
  helpers.
- `replay_mb_match.[ch]`: temporal and spatial candidate generation/scoring.
- `replay_mb_encode.[ch]`: frame loop, block loop, candidate selection.
- `replay_mb_decode_ref.[ch]`: small decoder/verifier for generated blocks.
- `replay_mb_trace.[ch]`: optional debug trace of block decisions.
- `mb7_codec.[ch]`, `mb17_codec.[ch]`, `mb19_codec.[ch]`, `mb20_codec.[ch]`:
  per-format bitstream rules.

The trace module is important for a self-documenting encoder. It should be
possible to ask the encoder why a block was emitted as stationary, motion,
4x4 data, or split 2x2.

## Data Structures

Prefer plain structs with explicit ownership:

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

typedef enum {
    REPLAY_MB_MODE_STATIONARY,
    REPLAY_MB_MODE_TEMPORAL,
    REPLAY_MB_MODE_SPATIAL,
    REPLAY_MB_MODE_DATA_4X4,
    REPLAY_MB_MODE_SPLIT_2X2
} ReplayMbMode;

typedef struct {
    ReplayMbMode mode;
    int bit_cost;
    int error;
    int dx;
    int dy;
} ReplayMbCandidate;
```

The implementation can pack pixels later if profiling shows it matters. The
first version should favour clarity over memory density.

## Trace Format

The trace output should be text first. It is more useful for reverse
engineering than a binary diagnostics stream.

Useful per-frame trace fields:

- frame number;
- selected codec;
- input and working pixel formats;
- starting loss level;
- retry count;
- final byte count;
- target window;
- aggregate mode counts.

Useful per-block trace fields:

```text
frame=12 block=40,28 mode=temporal dx=0 dy=-1 bits=6 error=18 threshold=44
frame=12 block=44,28 mode=data4x4 bits=71 error=0 yprev_in=17 yprev_out=19
frame=12 block=48,28 mode=split2x2 bits=64 error=9 reason=cheaper_than_4x4
```

The trace layer should sit beside candidate selection. It should not be mixed
into bit-writing routines.

## Verification Strategy

Build a decoder-side verifier before building the full search.

Minimum tests:

- bit writer writes known LSB-first patterns;
- Huffman writer emits every format-19 residual symbol correctly;
- data-coded 4x4 round-trips through the verifier;
- data-coded 2x2 round-trips through the verifier;
- stationary copy reconstructs from the previous frame;
- temporal and spatial copy offsets reconstruct the expected pixels;
- retrying a frame from the same source and loss level gives identical output.

The verifier does not need to be a complete Replay player. It only needs enough
format-specific decode logic to prove that the encoder's reconstructed frame
matches what a decoder would produce.

Later validation can compare generated frame payloads against the Acorn
decompressor or a RISC OS emulator, but the local C verifier should catch most
state-synchronisation mistakes first.

## Replay Container Work

Treat the codec payload and the Replay file/container as separate milestones.

First milestone:

- emit one compressed frame payload;
- verify it with the local decoder.

Second milestone:

- emit a sequence of compressed frame payloads;
- maintain previous-frame state;
- dump reconstructed frames for inspection.

Third milestone:

- write a minimal Replay movie with the correct compression type, frame size,
  frame rate, chunk layout, and key-frame data.

The earlier CompLib notes should guide this work, but the new C code does not
need to reproduce CompLib's structure exactly. CompLib is useful as a source of
policies: chunk sizes, key-frame handling, frame retry, and command-line
meaning.

## Suggested Build Order

1. Implement growable buffers and the LSB-first bit writer.
2. Add golden tests for bit ordering.
3. Add the format descriptor skeleton for formats 7, 17, 19, and 20, with only
   format 19 enabled.
4. Implement format-19 Huffman writing and tests for every residual symbol.
5. Implement format-19 data-coded 4x4 and 2x2 blocks.
6. Implement a minimal verifier that decodes those blocks into a frame buffer.
7. Add raw `rgb24` stdin/file input and RGB-to-`6Y5UV` conversion.
8. Add command-line `--data-only` mode.
9. Add stationary blocks.
10. Add temporal block search.
11. Add spatial block search.
12. Add 4x4-versus-2x2 candidate selection.
13. Add frame retry control using `QP%`.
14. Add trace output and small visual/error reports.
15. Add minimal Replay container writing.
16. Add optional libav input support.

After step 6 the encoder should already be able to produce large but valid
format-19 data blocks. Every later step should reduce size without changing the
basic validation path.

## Optimisation Plan

Do not optimise the first implementation until it has a decoder-side verifier
and a traceable decision path.

Optimisations that preserve the bitstream:

- precompute Huffman bit lengths for residuals;
- cache source block luma/chroma averages;
- use early-exit error scoring;
- test stationary before broader motion search;
- order motion candidates by likely usefulness;
- cache legal spatial/temporal candidate offsets per block position;
- use packed pixels or structure-of-arrays if profiling shows frame access is
  hot;
- add SIMD for colour conversion and error scoring after the scalar code is
  correct.

Possible quality improvements within the same format:

- use rate/distortion scoring instead of threshold-only matching;
- tune luma/chroma error weights;
- choose chroma values by minimising quantised reconstruction error;
- use scene-change detection to force cleaner key frames;
- expose separate controls for loss level, search radius, and target bitrate.

Non-compatible improvements to avoid in the first encoder:

- new Huffman tables;
- arithmetic coding;
- larger or variable-size blocks;
- sub-pixel motion;
- bidirectional prediction;
- inter-frame reference queues.

Those may be interesting for a new codec, but Replay players cannot decode
them as format 19.

## First Command-Line Shape

A minimal compressor tool could look like:

```text
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 \
    -pix_fmt rgb24 -f rawvideo - | replay-enc \
    --input - --input-format rgb24 --size 320x256 --fps 12.5 \
    --codec 19 --output movie,replay --quality 8 \
    --trace decisions.txt
```

For clarity, the internal option should be named `loss_level`, but the command
line may accept `--quality` if that matches Acorn terminology.

Useful development options:

- `--data-only`: emit only data-coded blocks;
- `--no-spatial`: disable spatial candidates;
- `--no-motion`: disable non-stationary temporal candidates;
- `--block-trace x,y`: explain one block in detail;
- `--dump-recon frame.ppm`: write the reconstructed frame used by the encoder.
- `--ffmpeg-input`: use the optional libav input path when compiled in.

These switches will make it much easier to compare the C encoder with the
original source and with the Replay decompressor.

## Implemented Format-19 Encoder

The encoder now lives in `replay-tooling` and implements:

- `mb_color_rgb24_to_6y5uv`, using CompLib's non-dithered fixed-point path;
- `codec_supermovingblocks_encode_data_frame`, emitting format-19 4x4 data
  blocks only;
- the general `codec_supermovingblocks_encode_frame` path with stationary,
  temporal, spatial, and split decisions;
- all 29 source-derived matching levels and whole-frame target-byte retries;
- block-average signed five-bit U and V values;
- the documented top-row, vertical, and left/above-average luma predictors;
- raw byte-padded payload output;
- immediate decode and reconstructed-frame comparison;
- a one-line frame decision trace;
- reconstructed-frame PPM preview output.

Single-payload mode deliberately handles exactly one RGB24 frame:

```text
replay-encode --codec 19 --input FILE|- --size WIDTHxHEIGHT \
    --payload FRAME.mb19 [--trace FRAME.trace] [--recon-ppm FRAME.ppm]
```

Requiring EOF after the frame prevents accidental concatenation of
variable-length payloads without a container. Multi-frame input uses the
explicit numbered payload sequence below.

The CLI also supports a raw-payload sequence without defining a temporary
container:

```text
replay-encode --codec 19 --input FILE|- --size WIDTHxHEIGHT \
    --payload-prefix PREFIX [--frames N] [--data-only] [--loss-level 0..28]
    [--target-bytes N]
```

Each payload is written as `PREFIX000000.mb19`, `PREFIX000001.mb19`, and so
on. The first frame has no temporal reference, but may use spatial and split
blocks. Later frames may also use stationary and temporal blocks against the
previous reconstructed frame. Level 0 requires an exact match to the current
data-block reconstruction target; levels 1 through 28 use the four-column
`QP%` threshold table copied from the original compressor.

The encoder keeps predictor state unchanged for stationary blocks, matching
the decoder. Mode statistics report meaningful bits and separate data/still
block counts for all implemented mode families.

Temporal 4x4 matching is also implemented. Candidates are enumerated in
increasing encoded length: radius one, radius two, radius three, then the far
table, preserving table order within each family. The accepted candidate with
the lowest threshold error is retained; table order resolves equal errors.
Temporal matching uses the same data-block reconstruction target and quality
metric as stationary matching.

The CLI now implements the source-style whole-frame retry policy as shared
`mb_rate_control` code rather than embedding it in the format-19 codec. A
target produces the original broad 90% to 102.5% size window. Each retry moves
one quality-table row in the required direction, never reverses direction for
that frame, and verifies the candidate payload before considering another
attempt. As in the original compressor, the first frame is accepted without a
rate retry and the accepted loss level carries into the next frame. The
chunk-budget `Cut` escape path is intentionally deferred until a Replay
container supplies real chunk accounting.

Spatial 4x4 matching uses the eight legal format-19 vectors and the selected
quality row. It reads only pixels already reconstructed in raster order, so it
is valid in both key frames and inter frames. In MPEG terminology, Replay key
frames are I-frame-like; spatial copies do not make them dependent on another
frame. Replay inter frames are P-frame-like because stationary and temporal
blocks can reference the previous reconstruction.

Split selection builds two real bitstream candidates from the same
incoming predictor state: one 4x4 data block, or four 2x2 blocks chosen from
threshold-matched stationary, temporal, spatial, and data modes. It compares
meaningful emitted bits, commits the selected candidate, and retains that candidate's
predictor state. Pure four-data splits normally lose because of their extra
headers; mixed splits can win when quadrants use compact copy modes.

Spatial references between quadrants make candidate evaluation stateful. The
encoder therefore maintains a private 4x4 tentative reconstruction while it
builds the split candidate. A spatial source inside that 4x4 region is visible
only after an earlier quadrant has produced it. This mirrors decoder order
without modifying the live reconstructed frame when the competing 4x4 data
candidate ultimately wins.

The CompLib coefficients are represented by rounded 16.16 integer constants.
This is source-derived but not yet proven byte-for-byte against the ARM build;
an emulator-generated RGB conversion fixture should settle any one-unit
rounding differences and whether optional dithering was enabled by a given
compressor configuration.

The generated payload path is now independently checked against the compiled
Acorn `Decomp19/Decompress,ffd` through Unicorn. Focused stationary, temporal,
spatial, split, and lossy fixtures produce byte-identical decoded `6Y5UV`,
including temporal and spatial 2x2 blocks. The harness follows `CodecIf`, leaves
the colour patch table untouched, and emulates classic ARM alignment for four
block-header and two Huffman lookahead `LDMIA` instructions.

Encoder policy parity is a separate question from stream compatibility and is
not required. The C encoder currently prioritises stationary, then temporal,
then spatial copy families; temporal and spatial searches choose the lowest
accepted error. The original compressor compares errors across a broader
search. Original-compressor traces should quantify and document the resulting
bitrate, decoder-visible error, and reference-propagation differences.
