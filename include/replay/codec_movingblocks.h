#ifndef CODEC_MOVINGBLOCKS_H
#define CODEC_MOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_encode.h"
#include "replay/mb_frame.h"
#include "replay/replay_buffer.h"

/*
 * Compression type 7, the original "Moving Blocks". YUV555 working pixels like
 * type 17, but data blocks are literal: a 4x4 block is sixteen 5-bit Y values
 * plus one 5-bit U and V (90 bits), a 2x2 block four Y plus U and V (30 bits) --
 * no luma predictor or Huffman. Its block grammar and motion coding also differ
 * from types 17/19 (see Decomp7/Docs/Stream): variable-length top-level codes
 * (`1` data, `00` move, `01` split) with no separate stationary opcode, and a
 * +/-4 motion family.
 *
 * The data primitives read only the block payload; the frame verifier consumes
 * the variable-length prefix and motion codes.
 */
ReplayStatus codec_movingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPixel *pixels, size_t stride,
    MbVerifyError *error);

ReplayStatus codec_movingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPixel *pixels, size_t stride,
    MbVerifyError *error);

/* Decode and strictly validate one complete type 7 frame payload. */
ReplayStatus codec_movingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

/*
 * Enabled block decisions and quality state for one type 7 frame. Type 7 has no
 * separate stationary opcode -- a stationary copy is a temporal move with a zero
 * vector -- but the encoder still scores it independently, so the switch is kept
 * for symmetry with the rest of the Moving Blocks family. The motion family is
 * +/-4 and data blocks are literal, but the copy/quality model is shared with
 * types 17 and 19, so loss_level, policy and the search workspace carry over.
 */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    unsigned loss_level;
    MbEncodePolicy policy;
    MbEncodeWorkspace *workspace; /* optional temporal-search cache */
} CodecMovingBlocksEncodeOptions;

/* Per-frame block tally (the stationary counters fold into temporal moves). */
typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
    size_t spatial4x4_blocks;
    size_t split4x4_blocks;
    size_t data2x2_blocks;
    size_t stationary2x2_blocks;
    size_t temporal2x2_blocks;
    size_t spatial2x2_blocks;
    size_t bits_written;
} CodecMovingBlocksEncodeStats;

/*
 * Encode one frame to a type 7 payload. `source` holds quantised YUV555 working
 * pixels; `reconstructed` receives exactly what a type 7 decoder retains and is
 * the `previous` for the next inter frame. `output` is cleared on entry. Caller
 * validates the source range. `stats` is optional.
 */
ReplayStatus codec_movingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksEncodeStats *stats);

#endif
