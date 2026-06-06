#include "replay/mb_codec.h"

/*
 * Type 17 keeps the original YUV555 working pixels but extends the motion
 * range. It is a distinct bitstream variant, not merely a quality option for
 * type 7, so it retains a separate codec module.
 */
const MbCodec codec_movingblockshq = {
    REPLAY_CODEC_MOVINGBLOCKSHQ,
    "Moving Blocks HQ",
    MB_WORK_YUV555,
    5, 5, 5,
    4, 4,
    8, 8,
    NULL,
    0, 0
};
