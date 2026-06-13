# C Tooling Libraries And Dependencies

This note records dependency choices for the portable Replay compressor tools.
The default policy is to keep the codec core small, explicit, and easy to
audit. Libraries should remove incidental work, not hide codec state or
bitstream layout.

## Byte Buffers

A growable byte buffer is useful, but the project does not need a large
container library.

Reasonable options:

- write a small `ReplayBuffer` locally;
- use `kvec` from `klib` if a generic dynamic-array helper is wanted;
- avoid string libraries such as SDS for compressed payloads.

Recommendation: write `ReplayBuffer` locally.

The required API is tiny:

```c
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ReplayBuffer;

int replay_buffer_reserve(ReplayBuffer *buf, size_t capacity);
int replay_buffer_append_u8(ReplayBuffer *buf, uint8_t value);
int replay_buffer_append(ReplayBuffer *buf, const void *data, size_t size);
void replay_buffer_clear(ReplayBuffer *buf);
void replay_buffer_free(ReplayBuffer *buf);
```

Local code is preferable here because:

- ownership is obvious;
- failure paths can match the rest of the tool;
- the buffer can later support bit-writer rollback or frame retry snapshots;
- the implementation should be only a few dozen lines.

`klib` is still worth remembering for later maps, heaps, sorting helpers, or
temporary vectors. Its `kvec.h` dynamic array is small and mature, but it is
not necessary for the first byte buffer.

SDS is binary-safe and mature, but it is fundamentally a dynamic string API.
Compressed frame payloads are not strings, and using a string-shaped API would
make the codec less self-documenting.

## Bitstreams

Recommendation: write Replay bitstream code locally.

Replay needs exact, format-specific bit ordering:

- Moving Blocks writes least-significant-bit-first bit fields;
- Moving Blocks data blocks mix fixed-width fields with fixed Huffman codes;
- Moving Lines is halfword-oriented rather than a conventional byte bitstream;
- verifier code needs to mirror the writer precisely.

Generic bitstream or serialization libraries are usually a poor fit because
they tend to encode their own assumptions about:

- most-significant-bit-first versus least-significant-bit-first order;
- byte flushing;
- signed value packing;
- stream abstraction;
- object serialization.

The local API should be deliberately small:

```c
typedef struct {
    ReplayBuffer *buffer;
    uint8_t bit_accum;
    unsigned bit_count;
} ReplayBitWriter;

void replay_bitwriter_init(ReplayBitWriter *bw, ReplayBuffer *buffer);
int replay_bitwriter_bits(ReplayBitWriter *bw, uint32_t value, unsigned count);
int replay_bitwriter_flush_zero(ReplayBitWriter *bw);
size_t replay_bitwriter_bitpos(const ReplayBitWriter *bw);
```

For Moving Blocks, `replay_bitwriter_bits()` should write the low `count` bits
of `value`, least-significant bit first.

Huffman writing should stay table-driven but explicit:

```c
typedef struct {
    uint16_t bits;
    uint8_t bit_count;
} ReplayHuffCode;

int replay_bitwriter_huff(ReplayBitWriter *bw,
                          const ReplayHuffCode *table,
                          unsigned symbol);
```

This keeps the fixed Replay tables visible in the source and makes tests easy:
each symbol can be written, decoded by the verifier, and compared with the
expected residual.

## Reader/Verifier Side

Add a matching local bit reader for verifier tests:

```c
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    uint8_t bit_accum;
    unsigned bit_count;
} ReplayBitReader;
```

The reader should be used for tests and payload verification, not in the hot
encoder path. Its main job is to prove that the writer and encoder
reconstruction match the decoder's interpretation.

Moving Lines should use separate halfword helpers:

```c
int replay_halfword_read_le(const uint8_t *data, size_t size,
                            size_t *pos, uint16_t *out);
int replay_halfword_write_le(ReplayBuffer *buffer, uint16_t value);
```

Do not force Moving Lines through the Moving Blocks bit writer. Its stream is a
sequence of 16-bit code words with packed literal runs, so sharing the
bit-level abstraction would make both codecs less clear.

## Other Libraries

Use external libraries where they solve a larger problem:

- FFmpeg CLI first for demuxing, decoding, scaling, frame-rate conversion, and
  colour conversion before raw `rgb24` reaches the compressor.
- Optional libav libraries later for direct video input.
- Unity or cmocka for C unit tests.
- libpng or `stb_image_write` only if PPM reconstructed-frame dumps become too
  limiting.
- SIMDe later for portable SIMD after scalar code is verified.

Avoid external libraries for:

- fixed Replay Huffman coding;
- block-mode bit writing;
- frame retry snapshots;
- codec candidate scoring.

Those are core codec behaviour and should be visible in the source.

## First Implementation Choice

Implement locally:

- `replay_buffer.[ch]`;
- `replay_bitwriter.[ch]`;
- `replay_bitreader.[ch]`;
- `replay_halfword.[ch]`.

The first tests should cover:

- appending bytes and reserving capacity;
- LSB-first writes across byte boundaries;
- zero flushing partial bytes;
- reading back the same fields;
- writing and reading little-endian halfwords;
- rejecting truncated streams cleanly.
