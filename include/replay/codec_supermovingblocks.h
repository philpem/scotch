#ifndef CODEC_SUPERMOVINGBLOCKS_H
#define CODEC_SUPERMOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

extern const MbHuffmanTable codec_supermovingblocks_luma_huffman;

/*
 * These switches enable lossless alternatives to a 4x4 data block. A mode is
 * used only when it reconstructs exactly the same quantised pixels as the
 * corresponding data representation. Threshold-based lossy matching will be
 * a separate policy layer.
 *
 * Stationary and temporal modes require `previous`. Spatial mode only refers
 * to pixels already reconstructed in the current frame, so it is also legal
 * in a key frame. Split currently considers 2x2 data and stationary modes.
 */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
} CodecSuperMovingBlocksEncodeOptions;

/* Counts describe the selected stream, not the candidates considered. */
typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
    size_t spatial4x4_blocks;
    size_t split4x4_blocks;
    size_t data2x2_blocks;
    size_t stationary2x2_blocks;
    size_t bits_written;
} CodecSuperMovingBlocksEncodeStats;

ReplayStatus codec_supermovingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_supermovingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_supermovingblocks_write_data4x4(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[16]);

ReplayStatus codec_supermovingblocks_write_data2x2(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[4]);

/*
 * Encode a complete frame using only 4x4 data blocks. The reconstructed frame
 * receives the exact pixels a decoder will produce, including block-averaged
 * chroma. Output is replaced on entry and contains one byte-padded payload.
 */
ReplayStatus codec_supermovingblocks_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written);

/*
 * Encode one frame using the enabled lossless block decisions.
 *
 * `source` contains quantised 6Y5UV samples. `reconstructed` is filled with
 * exactly what a format-19 decoder will retain and must therefore be supplied
 * as `previous` for the next inter frame. `output` is cleared even when source
 * validation fails, preserving replacement semantics.
 */
ReplayStatus codec_supermovingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecSuperMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecSuperMovingBlocksEncodeStats *stats);

ReplayStatus codec_supermovingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

#endif
