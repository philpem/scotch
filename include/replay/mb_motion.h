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

/*
 * Read the type 7 move grammar (Decomp7/Docs/Stream): a two-bit family then an
 * index -- `00` stationary, `01`+3 temporal radius 1, `10`+4 temporal radius 2,
 * `11`+6 temporal radius 4 (index 0..31) / radius 3 (32..55) / spatial (56..63).
 * The caller has consumed the move prefix (`00` 4x4 or `0` 2x2). The +/-4 family
 * differs from the 17/19 coding, but the vectors are the shared rings and
 * spatial tables. `motion->spatial` distinguishes the previous-frame copies from
 * the current-frame ones.
 */
ReplayStatus mb_motion_read_format7(ReplayBitReader *reader,
                                    MbMotionBlockSize block_size,
                                    MbMotionVector *motion);

/* Write the type 7 move code after the caller has emitted the move opcode. */
ReplayStatus mb_motion_write_format7(ReplayBitWriter *writer,
                                     MbMotionBlockSize block_size,
                                     const MbMotionVector *motion);

/*
 * Enumerate the type 7 candidate vectors for the encoder's motion search, in
 * non-decreasing code length. Temporal covers radii 1..4 (80 vectors); spatial
 * is the eight source-defined references. The stationary (0,0) code is tested
 * separately and is not part of the temporal enumeration.
 */
ReplayStatus mb_motion_format7_temporal_at(unsigned index,
                                           MbMotionVector *motion);

ReplayStatus mb_motion_format7_spatial_at(MbMotionBlockSize block_size,
                                          unsigned index,
                                          MbMotionVector *motion);

#endif
