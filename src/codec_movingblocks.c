#include "replay/mb_codec.h"

/*
 * Compression type 7 is the original Moving Blocks format. This descriptor is
 * an implementation signpost: it records the source-derived working precision
 * and nominal search range while its bitstream core remains unimplemented.
 */
const MbCodec codec_movingblocks = {
    REPLAY_CODEC_MOVINGBLOCKS,
    "Moving Blocks",
    MB_WORK_YUV555,
    5, 5, 5,
    4, 4,
    4, 4,
    NULL,
    0, 0
};
