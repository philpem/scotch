#ifndef MB_MOTION_H
#define MB_MOTION_H

#include "replay/replay_bitstream.h"

typedef enum {
    MB_MOTION_BLOCK_2X2 = 2,
    MB_MOTION_BLOCK_4X4 = 4
} MbMotionBlockSize;

typedef struct {
    int dx;
    int dy;
    int spatial;
} MbMotionVector;

/* The caller has already consumed the 4x4 `01` or 2x2 `1` move prefix. */
ReplayStatus mb_motion_read_format19(ReplayBitReader *reader,
                                     MbMotionBlockSize block_size,
                                     MbMotionVector *motion);

#endif

