#include "replay/mb_codec.h"

/*
 * Type 20 is the 6Y6UV beta branch. Its wider chroma precision changes data
 * representation and tables, so format-19 code must not silently handle it.
 */
const MbCodec codec_movingblocksbeta = {
    REPLAY_CODEC_MOVINGBLOCKSBETA,
    "Moving Blocks Beta",
    MB_WORK_6Y6UV,
    6, 6, 6,
    4, 4,
    8, 8,
    NULL,
    0, 0
};
