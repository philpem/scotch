#ifndef CODEC_SUPERMOVINGBLOCKS_H
#define CODEC_SUPERMOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

extern const MbHuffmanTable codec_supermovingblocks_luma_huffman;

ReplayStatus codec_supermovingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_supermovingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

#endif
