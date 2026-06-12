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

/* Decode and strictly validate one complete type 17 frame payload. */
ReplayStatus codec_movingblockshq_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

#endif
