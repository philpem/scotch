#ifndef CODEC_SUPERMOVINGBLOCKS_H
#define CODEC_SUPERMOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

extern const MbHuffmanTable codec_supermovingblocks_luma_huffman;

typedef struct {
    int allow_stationary;
    int allow_temporal;
} CodecSuperMovingBlocksEncodeOptions;

typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
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

/* Encode one frame using the enabled lossless block decisions. */
ReplayStatus codec_supermovingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecSuperMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecSuperMovingBlocksEncodeStats *stats);

ReplayStatus codec_supermovingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

#endif
