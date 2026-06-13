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

/* Decode and strictly validate one complete type 17 frame payload. */
ReplayStatus codec_movingblockshq_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

#endif
