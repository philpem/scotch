#ifndef MB_FRAME_VERIFY_H
#define MB_FRAME_VERIFY_H

#include "replay/mb_frame.h"
#include "replay/replay_bitstream.h"

typedef enum {
    MB_DECODE_MODE_DATA,
    MB_DECODE_MODE_STATIONARY,
    MB_DECODE_MODE_TEMPORAL,
    MB_DECODE_MODE_SPATIAL,
    MB_DECODE_MODE_SPLIT
} MbDecodeMode;

typedef struct {
    unsigned x;
    unsigned y;
    unsigned size;
    MbDecodeMode mode;
    /* Stream range [bit_start, bit_end), including the block opcode. */
    size_t bit_start;
    size_t bit_end;
    /* Meaningful only for temporal and spatial events. */
    int motion_dx;
    int motion_dy;
} MbDecodeEvent;

typedef void (*MbDecodeTrace)(const MbDecodeEvent *event, void *opaque);

typedef ReplayStatus (*MbDataBlockDecoder)(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

typedef struct {
    MbDataBlockDecoder decode_data4x4;
    MbDataBlockDecoder decode_data2x2;
} MbFrameVerifyCodec;

/*
 * Verify the frame grammar shared by Moving Blocks HQ-derived formats.
 * Types 17 and 19 use the same stationary, motion, spatial, split, scan, and
 * padding rules; codec callbacks supply their different data reconstruction.
 */
ReplayStatus mb_frame_verify(
    const MbFrameVerifyCodec *codec,
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    MbDecodeTrace trace, void *trace_opaque);

#endif
