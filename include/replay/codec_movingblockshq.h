#ifndef CODEC_MOVINGBLOCKSHQ_H
#define CODEC_MOVINGBLOCKSHQ_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

extern const MbHuffmanTable codec_movingblockshq_luma_huffman;

/*
 * Decode one type 17 data block using only the frame's shared luma predictor.
 * The functions consume the complete case prefix: top-level `10` for 4x4 and
 * split-child `01` for 2x2, written here in the order bits reach the decoder.
 *
 * Each block then contains one five-bit U value, one five-bit V value, and a
 * Huffman residual for every luma sample. Successful decoding updates
 * predictor->luma to the reconstructed block average, exactly as Decomp17
 * does. Callers must therefore invoke these functions in frame scan order.
 */
ReplayStatus codec_movingblockshq_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_movingblockshq_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

/*
 * Encode one source block (YUV555) as a data-coded 4x4 or split-child 2x2,
 * choosing the block's 5-bit average chroma and Huffman-coding the luma
 * prediction residuals (lossless luma). `recon` receives the reconstructed
 * block and `predictor` is carried to the next data block, mirroring the
 * matching decode functions so encode/decode round-trip exactly.
 */
ReplayStatus codec_movingblockshq_encode_data4x4(
    ReplayBitWriter *writer, const MbPixel *source, MbPixel *recon,
    size_t stride, MbPredictor *predictor);

ReplayStatus codec_movingblockshq_encode_data2x2(
    ReplayBitWriter *writer, const MbPixel *source, MbPixel *recon,
    size_t stride, MbPredictor *predictor);

/*
 * Encode a complete type 17 frame using only 4x4 data blocks. Every block is
 * Huffman-coded with lossless luma and 5-bit average chroma, so `reconstructed`
 * receives exactly the YUV555 pixels Decomp17 will produce. `output` is cleared
 * on entry and left byte-padded. `source` and `reconstructed` must share their
 * stride (the data-block primitive predicts across one pitch). This is the
 * key-frame-capable baseline; copy modes are added separately.
 */
ReplayStatus codec_movingblockshq_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written);

/*
 * Enabled block decisions for codec_movingblockshq_encode_frame. Stationary and
 * temporal copies reference the previous decoded frame and so require it;
 * spatial copies reference already-reconstructed current-frame pixels and are
 * legal in a key frame. `loss_level` selects the shared QP% acceptance row
 * (0 = exact). Copy families are tried stationary, then temporal, then spatial,
 * matching the original compressor's ordered policy.
 */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    unsigned loss_level;
} CodecMovingBlocksHqEncodeOptions;

/* Block tally for the selected stream; bits_written excludes byte padding. */
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
} CodecMovingBlocksHqEncodeStats;

/*
 * Encode one type 17 frame using the enabled block decisions, driven by the
 * shared mb_encode motion search and the YUV555 quality model. `source` holds
 * quantised YUV555 samples; `reconstructed` is filled with exactly what
 * Decomp17 retains and must be supplied as `previous` for the next inter frame.
 * `source` and `reconstructed` must share their stride. `output` is cleared on
 * entry even when validation fails. `stats` is optional.
 */
ReplayStatus codec_movingblockshq_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksHqEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksHqEncodeStats *stats);

/* Decode and strictly validate one complete type 17 frame payload. */
ReplayStatus codec_movingblockshq_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

#endif
