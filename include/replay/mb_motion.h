#ifndef MB_MOTION_H
#define MB_MOTION_H

#include "replay/replay_bitstream.h"

typedef enum {
    MB_MOTION_BLOCK_2X2 = 2,
    MB_MOTION_BLOCK_4X4 = 4
} MbMotionBlockSize;

typedef struct {
    /* Offset is relative to the destination block's top-left pixel. */
    int dx;
    int dy;
    /* Zero selects the previous frame; non-zero selects the current frame. */
    int spatial;
} MbMotionVector;

/*
 * Read the motion grammar shared by types 17 and 19 (and inherited by type
 * 20). The caller has consumed the 4x4 `01` or 2x2 `1` move prefix.
 */
ReplayStatus mb_motion_read_hq(ReplayBitReader *reader,
                               MbMotionBlockSize block_size,
                               MbMotionVector *motion);

/* Historical type-19 name retained for source compatibility. */
ReplayStatus mb_motion_read_format19(ReplayBitReader *reader,
                                     MbMotionBlockSize block_size,
                                     MbMotionVector *motion);

/* Write the family/index after the caller has written the move prefix. */
ReplayStatus mb_motion_write_format19(ReplayBitWriter *writer,
                                      MbMotionBlockSize block_size,
                                      const MbMotionVector *motion);

/* Enumerate temporal vectors in increasing code-length and table order. */
ReplayStatus mb_motion_format19_temporal_at(unsigned index,
                                            MbMotionVector *motion);

/* Enumerate the eight source-defined spatial vectors in bitstream order. */
ReplayStatus mb_motion_format19_spatial_at(MbMotionBlockSize block_size,
                                           unsigned index,
                                           MbMotionVector *motion);

#endif
