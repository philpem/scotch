#include "replay/mb_codec.h"

const MbCodec *mb_codec_find(unsigned id)
{
    switch (id) {
    case REPLAY_CODEC_MOVINGBLOCKS:
        return &codec_movingblocks;
    case REPLAY_CODEC_MOVINGBLOCKSHQ:
        return &codec_movingblockshq;
    case REPLAY_CODEC_SUPERMOVINGBLOCKS:
        return &codec_supermovingblocks;
    case REPLAY_CODEC_MOVINGBLOCKSBETA:
        return &codec_movingblocksbeta;
    default:
        return NULL;
    }
}

