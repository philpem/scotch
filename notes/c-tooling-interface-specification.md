# C Tooling Interface Specification

This note closes the remaining interface decisions needed before coding. It is
the implementation contract for the first portable C tools.

## Codec Descriptor

Moving Blocks formats share the outer frame loop but keep format-specific
tables and data-block state behind a descriptor:

```c
typedef enum {
    REPLAY_CODEC_MOVING_BLOCKS_7 = 7,
    REPLAY_CODEC_MOVING_BLOCKS_HQ = 17,
    REPLAY_CODEC_SUPER_MOVING_BLOCKS = 19,
    REPLAY_CODEC_MOVING_BLOCKS_BETA = 20
} ReplayCodecId;

typedef enum {
    REPLAY_WORK_YUV555,
    REPLAY_WORK_6Y5UV,
    REPLAY_WORK_6Y6UV
} ReplayWorkingFormat;

typedef struct ReplayMbCodec {
    ReplayCodecId id;
    const char *name;
    ReplayWorkingFormat working_format;
    uint8_t y_bits, u_bits, v_bits;
    uint8_t block_width, block_height;
    uint8_t max_motion_x, max_motion_y;
    const ReplayHuffmanTable *luma_huffman;
    const ReplayMotionTable *motion_4x4;
    const ReplayMotionTable *motion_2x2;
    void (*reset_frame_state)(ReplayMbState *state);
    ReplayStatus (*write_data_4x4)(ReplayMbEncoder *, int x, int y);
    ReplayStatus (*write_data_2x2)(ReplayMbEncoder *, int x, int y);
} ReplayMbCodec;
```

Only format 19 is enabled initially. Descriptors for 7, 17, and 20 may exist
as metadata, but selecting an unimplemented encoder returns
`REPLAY_UNSUPPORTED_CODEC`.

Search code asks the descriptor which candidates are legal and their exact bit
cost. It must not contain format-19 opcodes inline.

## Frames And Pixel Input

Keep raw input and codec working frames distinct:

```c
typedef enum {
    REPLAY_INPUT_RGB24,
    REPLAY_INPUT_RGBA32,
    REPLAY_INPUT_GRAY8
} ReplayInputFormat;

typedef struct {
    ReplayInputFormat format;
    const uint8_t *data;
    size_t stride;
} ReplayInputFrame;

typedef struct {
    uint8_t y, u, v;
} ReplayPixel;

typedef struct {
    int width, height;
    size_t stride;
    ReplayPixel *pixels;
} ReplayFrame;
```

V1 accepts `RGB24` only in the CLI. The other enum values reserve a stable API
shape and remain unsupported until tested. `stride` is bytes for input frames
and pixels for working frames.

Dimensions must be positive and divisible by four for Moving Blocks. Reject
partial input frames and arithmetic overflow before allocating.

## Colour Conversion Policy

V1 matches CompLib's RGB-to-YUV path rather than delegating conversion to an
unspecified platform routine. For 8-bit RGB input, use fixed-point equivalents
of the source coefficients:

```text
Y = 0.299 R + 0.587 G + 0.114 B
U = (B - Y) / (2 * 0.886)
V = (R - Y) / (2 * 0.701)
```

Then apply CompLib-compatible rounding to the selected working precision. For
format 19, map Y to `0..63` and signed U/V to five-bit modulo values. Keep this
conversion in a separately tested module so a future explicit BT.601 limited-
range policy can be added without changing codec code.

Do not ask FFmpeg to output Replay's packed YUV representation in v1. FFmpeg
supplies `rgb24`; the project owns and tests the final Replay quantisation.

## Raw Frame Source

The raw reader has no container metadata, so every required property is
explicit:

```text
--input FILE|-            required; `-` means stdin
--input-format rgb24      required in v1
--size WIDTHxHEIGHT       required
--fps NUM/DEN             required for Replay output, optional for payload tests
```

One frame is exactly `width * height * 3` bytes. EOF at a frame boundary is
normal. EOF inside a frame is an error that reports the frame number and byte
count. The reader does not seek and therefore works identically with files and
FFmpeg pipes.

The supported first workflow is:

```text
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 \
  -pix_fmt rgb24 -f rawvideo - \
  | replay-enc --input - --input-format rgb24 --size 320x256 \
      --fps 25/2 --codec 19 --output movie,replay
```

## FFmpeg Library Decision

Do not link libav in v1. Add it only after all of these are true:

- raw `rgb24` input encodes multiple frames;
- format-19 payload self-verification passes;
- a video-only Replay file plays in an existing implementation;
- the codec API no longer depends on CLI-owned buffers.

The future libav module implements the same frame-source callback as the raw
reader. It may use `libavformat`, `libavcodec`, and `libswscale`; it must not
move scaling, timing, or ownership concerns into the codec core.

## Bitstream Interfaces

Moving Blocks uses a local LSB-first writer:

```c
typedef struct {
    ReplayBuffer *buffer;
    uint64_t accumulator;
    unsigned bit_count;
    size_t total_bits;
} ReplayBitWriter;

ReplayStatus replay_bits_write(ReplayBitWriter *, uint32_t value,
                               unsigned count);
ReplayStatus replay_bits_flush_zero(ReplayBitWriter *);
size_t replay_bits_position(const ReplayBitWriter *);
```

`replay_bits_write` emits the low `count` bits of `value`, least-significant
bit first. Valid counts are `0..32`; count zero is a no-op. Flush pads the final
byte with zero bits and is called only at a format-defined frame boundary.

The verifier has an independent reader API with explicit end-of-input errors.
Moving Lines uses separate little-endian halfword helpers and never passes
through this bit writer.

## Encoder And Verification API

```c
typedef struct {
    ReplayCodecId codec_id;
    int width, height;
    unsigned loss_level;
    size_t target_bytes_per_frame;
    unsigned flags;
} ReplayEncoderConfig;

typedef struct {
    uint8_t *data;
    size_t size, capacity;
} ReplayPacket;

ReplayStatus replay_encoder_encode_frame(ReplayEncoder *,
                                         const ReplayInputFrame *,
                                         ReplayPacket *);

ReplayStatus replay_verify_payload(const ReplayMbCodec *,
                                   int width, int height,
                                   const ReplayPacket *,
                                   const ReplayFrame *previous,
                                   ReplayFrame *decoded,
                                   ReplayVerifyError *error);
```

The encoder always keeps its own reconstructed frame. With
`--verify-payload`, it decodes each emitted packet and compares every component
against that frame before accepting it.

## Trace Format

V1 trace output is stable, line-oriented text. It is easy to diff and does not
require a JSON library.

Each line begins with a record kind:

```text
frame frame=12 retry=1 loss=8 bytes=3476 target_min=3200 target_max=3600
block frame=12 x=40 y=28 size=4 mode=temporal dx=0 dy=-1 bits=6 error=18
block frame=12 x=44 y=28 size=4 mode=data bits=71 error=0 ypred_in=17 ypred_out=19
verify frame=12 status=ok bits_consumed=27808
```

Required frame fields are frame number, retry, loss level, byte count, and mode
counts. Required block fields are position, block size, selected mode, exact
bit cost, reconstruction error, and motion offset when applicable.

Trace failures are fatal only when the user explicitly names a trace file and
that file cannot be opened or written. Tracing must not affect codec choices.

## Error Policy

Public functions return a `ReplayStatus` enum, never print, and leave detailed
CLI wording to the tools. Distinguish at least:

- invalid argument;
- allocation failure;
- truncated input;
- malformed payload;
- output overflow;
- unsupported codec or pixel format;
- reconstruction mismatch;
- I/O failure.

Malformed-stream errors include bit position, frame number, and block
coordinates when known. No function silently accepts a partial frame.
