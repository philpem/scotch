#ifndef MB_ENCODE_H
#define MB_ENCODE_H

#include <stdint.h>

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

/*
 * Codec hook table for the shared motion search. The search enumerates
 * candidate vectors and scores block copies; everything codec-specific -- the
 * candidate vector tables, the block-match distortion model, and the number of
 * quality levels the temporal cache spans -- is reached through this table.
 * Type 19 fills it from its 6Y5UV model; type 17 will supply a YUV555 one.
 *
 * The search never interprets `quality`: it only forwards the pointer its
 * caller passed straight to the codec's block_match, so the concrete threshold
 * type stays private to the codec.
 */
typedef struct MbEncodeCodec {
    unsigned level_count;     /* quality levels the temporal cache spans */
    unsigned temporal_count;  /* number of temporal candidate vectors */
    unsigned spatial_count;   /* number of spatial candidate vectors */

    /* Fetch a candidate vector by index. Returns nonzero on success. */
    int (*temporal_vector)(unsigned index, MbMotionVector *out);
    int (*spatial_vector)(MbMotionBlockSize block_size, unsigned index,
                          MbMotionVector *out);

    /*
     * Threshold match used by the stationary and spatial paths: nonzero when
     * the copy is accepted within `quality`, setting *error. The spatial child
     * pixels are supplied through `reference` at (ref_x, ref_y).
     */
    int (*block_match)(const MbFrame *source, unsigned x, unsigned y,
                       const MbFrame *reference, unsigned ref_x,
                       unsigned ref_y, unsigned size, uint8_t u, uint8_t v,
                       const void *quality, unsigned *error);

    /*
     * Profile match driving the temporal cache: nonzero when a copy profile
     * exists, setting *total_error and *first_level (the lowest quality level
     * at which the copy is accepted, or level_count if it never is).
     */
    int (*profile_match)(const MbFrame *source, unsigned x, unsigned y,
                         const MbFrame *reference, unsigned ref_x,
                         unsigned ref_y, unsigned size, uint8_t u, uint8_t v,
                         unsigned *total_error, unsigned *first_level);
} MbEncodeCodec;

#endif
