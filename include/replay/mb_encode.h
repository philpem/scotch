#ifndef MB_ENCODE_H
#define MB_ENCODE_H

#include "replay/mb_frame.h"
#include "replay/mb_motion.h"

/*
 * Codec-neutral encoder primitives shared by the Moving Blocks family (types
 * 17, 19, ...). The block-decision modes, the per-block copy candidate, and the
 * metrics used to choose between candidates do not depend on a codec's data
 * block coding, so they live here for the type-specific encoders to share.
 */

/* The decision made for one block (or split child). */
typedef enum {
    SPLIT_MODE_DATA,
    SPLIT_MODE_STATIONARY,
    SPLIT_MODE_TEMPORAL,
    SPLIT_MODE_SPATIAL
} SplitMode;

/* A candidate decision for one block, compared lowest-error first. */
typedef struct {
    SplitMode mode;
    MbMotionVector motion;
    unsigned error;   /* decoder-visible reconstruction error */
    unsigned bits;    /* emitted bit cost */
    unsigned order;   /* enumeration order, the final tie-break */
    int valid;
} CopyCandidate;

/*
 * Emitted bit cost of a motion vector's copy code: the family/index payload
 * plus the case prefix (2 bits for a 4x4 block, 1 for a 2x2 split child). The
 * prefix widths are common to types 17 and 19.
 */
unsigned mb_encode_motion_bits(const MbMotionVector *motion,
                               MbMotionBlockSize block_size);

/*
 * True when `candidate` should replace `best`: it must be valid, then win on
 * lowest error, then fewest bits, then lowest enumeration order.
 */
int mb_encode_candidate_better(const CopyCandidate *candidate,
                               const CopyCandidate *best);

/*
 * Resolve a 2x2 spatial-copy source pixel during a split decision, enforcing
 * the shared 4x4 raster-scan legality: a source inside the current 4x4 parent
 * is valid only if `available_mask` marks it already decided in `tentative`; a
 * source outside the parent is valid only if its owning 4x4 block precedes the
 * parent in scan order. Returns the pixel, or NULL when the reference is not
 * yet reconstructed.
 */
const MbPixel *mb_encode_split_spatial_pixel(
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned block_x, unsigned block_y,
    unsigned source_x, unsigned source_y);

#endif
