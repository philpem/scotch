#ifndef MB_ENCODE_H
#define MB_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/mb_motion.h"
#include "replay/replay_bitstream.h"
#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

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

/*
 * Temporal search cache. The motion search is the same regardless of codec, so
 * the cache that lets it skip a recomputed vector across rate retries lives
 * here. Each block position holds the best accepted vector at every quality
 * level, filled once then reused. MB_ENCODE_MAX_LEVELS bounds the per-codec
 * level count (a codec's MbEncodeCodec.level_count must not exceed it).
 */
#define MB_ENCODE_MAX_LEVELS 29U

typedef struct {
    int8_t dx;
    int8_t dy;
    uint16_t error;
    uint8_t valid;
} MbEncodeTemporalResult;

typedef struct {
    MbEncodeTemporalResult levels[MB_ENCODE_MAX_LEVELS];
    uint8_t ready;
} MbEncodeTemporalEntry;

typedef struct {
    unsigned width;
    unsigned height;
    size_t count4x4;
    size_t count2x2;
    MbEncodeTemporalEntry *entries4x4;
    MbEncodeTemporalEntry *entries2x2;
} MbEncodeWorkspace;

/* Allocate the per-position caches for a width x height frame (both /4). */
ReplayStatus mb_encode_workspace_init(MbEncodeWorkspace *workspace,
                                      unsigned width, unsigned height);

/* Drop every cached vector so the next frame pair starts cold. */
void mb_encode_workspace_reset(MbEncodeWorkspace *workspace);

/* Release the caches; safe to call on a zeroed or already-destroyed value. */
void mb_encode_workspace_destroy(MbEncodeWorkspace *workspace);

/*
 * The shared Moving Blocks motion search. Each returns nonzero when a copy is
 * found, setting *motion, *matched_error and incrementing *evaluations per
 * compared candidate. The temporal searches consult `workspace` (NULL for an
 * uncached single search); the spatial searches score against `quality`, the
 * codec-private threshold pointer forwarded to MbEncodeCodec.block_match.
 */
int mb_encode_find_temporal4x4(const MbEncodeCodec *enc, const MbFrame *source,
                               const MbFrame *previous, unsigned x, unsigned y,
                               uint8_t u, uint8_t v, unsigned loss_level,
                               MbMotionVector *motion, unsigned *matched_error,
                               size_t *evaluations,
                               MbEncodeWorkspace *workspace);

int mb_encode_match_temporal2x2(const MbEncodeCodec *enc, const MbFrame *source,
                                const MbFrame *previous, unsigned x, unsigned y,
                                uint8_t u, uint8_t v, unsigned loss_level,
                                MbMotionVector *motion, unsigned *matched_error,
                                size_t *evaluations,
                                MbEncodeWorkspace *workspace);

int mb_encode_find_spatial4x4(const MbEncodeCodec *enc, const MbFrame *source,
                              const MbFrame *reconstructed, unsigned x,
                              unsigned y, uint8_t u, uint8_t v,
                              const void *quality, MbMotionVector *motion,
                              unsigned *matched_error, size_t *evaluations);

int mb_encode_match_spatial2x2(const MbEncodeCodec *enc, const MbFrame *source,
                               const MbFrame *reconstructed,
                               const MbPixel tentative[16],
                               unsigned available_mask, unsigned parent_x,
                               unsigned parent_y, unsigned x, unsigned y,
                               uint8_t u, uint8_t v, const void *quality,
                               MbMotionVector *motion, unsigned *matched_error,
                               size_t *evaluations);

/*
 * Reconstruct a size x size copy block: write `destination` at (x, y) from
 * `reference` at the motion offset. This covers every copy family -- temporal
 * (reference = previous frame), spatial (reference = the reconstruction in
 * progress), and stationary (previous frame with a zero vector) -- at both 4x4
 * and 2x2, identically for types 17 and 19.
 */
void mb_encode_copy_motion(const MbFrame *reference, MbFrame *destination,
                           unsigned x, unsigned y, unsigned size,
                           const MbMotionVector *motion);

/*
 * Record a 2x2 copy child's decoded pixels in the parent's tentative 4x4
 * reconstruction so a later spatial child can reference them. Spatial sources
 * inside the parent read earlier tentative pixels; temporal/stationary sources
 * and out-of-parent spatial sources read finished frames.
 */
void mb_encode_fill_tentative_copy2x2(
    const MbFrame *reference, const MbFrame *reconstructed,
    MbPixel tentative[16], unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, const MbMotionVector *motion,
    unsigned *available_mask);

/*
 * Append `bits` meaningful bits of a byte-padded candidate buffer to the live
 * stream, dropping the candidate's own zero padding.
 */
ReplayStatus mb_encode_append_candidate(ReplayBitWriter *writer,
                                        const ReplayBuffer *buffer,
                                        size_t bits);

#endif
