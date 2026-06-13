#ifndef CODEC_MOVINGBLOCKSBETA_H
#define CODEC_MOVINGBLOCKSBETA_H

#include "replay/mb_codec.h"
#include "replay/mb_encode.h"
#include "replay/mb_frame.h"
#include "replay/replay_buffer.h"

/*
 * Compression type 20, "Moving Blocks Beta": type 19 (Super Moving Blocks) with
 * 6Y6UV working pixels and delta-coded 6-bit chroma. The grammar, motion tables
 * and 64-symbol luma Huffman are identical to type 19; only the chroma differs.
 *
 * A data block stores one 8-bit "uv byte" (u-delta code in the low nibble, v in
 * the high nibble) in place of type 19's [U:5][V:5]. Each code indexes the
 * fixed delta table {-32,-26,-20,-14,-8,-4,-2,-1,0,1,2,4,8,14,20,26}; the result
 * is added to a chroma predictor carried across data blocks (like the luma
 * predictor) and masked to six bits. The predictor resets to zero each frame.
 */

ReplayStatus codec_movingblocksbeta_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_movingblocksbeta_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

/* Decode and strictly validate one complete type 20 frame payload. */
ReplayStatus codec_movingblocksbeta_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

/* Enabled block decisions and quality state for one type 20 frame. */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    unsigned loss_level;
    MbEncodePolicy policy;
    MbEncodeWorkspace *workspace;
} CodecMovingBlocksBetaEncodeOptions;

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
} CodecMovingBlocksBetaEncodeStats;

/*
 * Encode one frame to a type 20 payload. `source` holds quantised 6Y6UV working
 * pixels (Y 0..63, U/V 0..63 six-bit signed); `reconstructed` receives exactly
 * what a type 20 decoder retains and is the `previous` for the next inter frame.
 * `output` is cleared on entry. `stats` is optional.
 */
ReplayStatus codec_movingblocksbeta_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksBetaEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksBetaEncodeStats *stats);

#endif
