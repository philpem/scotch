#ifndef CODEC_MOVINGBLOCKS_H
#define CODEC_MOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

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

#endif
