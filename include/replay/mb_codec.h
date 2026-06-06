#ifndef MB_CODEC_H
#define MB_CODEC_H

#include <stdint.h>

#include "replay/mb_huffman.h"

typedef enum {
    /* Values are Replay compression type identifiers stored in movie metadata. */
    REPLAY_CODEC_MOVINGBLOCKS = 7,
    REPLAY_CODEC_MOVINGBLOCKSHQ = 17,
    REPLAY_CODEC_SUPERMOVINGBLOCKS = 19,
    REPLAY_CODEC_MOVINGBLOCKSBETA = 20
} MbCodecId;

typedef enum {
    /* Internal pixel precision expected by each original compressor. */
    MB_WORK_YUV555,
    MB_WORK_6Y5UV,
    MB_WORK_6Y6UV
} MbWorkingFormat;

typedef struct {
    /* Descriptive format facts, not a polymorphic encoder interface. */
    MbCodecId id;
    const char *name;
    MbWorkingFormat working_format;
    uint8_t y_bits;
    uint8_t u_bits;
    uint8_t v_bits;
    uint8_t block_width;
    uint8_t block_height;
    uint8_t max_motion_x;
    uint8_t max_motion_y;
    /* NULL until the corresponding source-derived table is implemented. */
    const MbHuffmanTable *luma_huffman;
    int encoder_implemented;
    int verifier_implemented;
} MbCodec;

const MbCodec *mb_codec_find(unsigned id);

extern const MbCodec codec_movingblocks;
extern const MbCodec codec_movingblockshq;
extern const MbCodec codec_supermovingblocks;
extern const MbCodec codec_movingblocksbeta;

#endif
