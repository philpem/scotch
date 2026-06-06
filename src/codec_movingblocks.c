#include "replay/mb_codec.h"

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

