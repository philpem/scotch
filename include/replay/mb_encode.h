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

/*
 * Signed average chroma of a size x size source block as a 5-bit value, the
 * value a data block would store. Floors like the source ASR; the Moving Blocks
 * family shares this signed five-bit chroma model.
 */
uint8_t mb_encode_average_chroma(const MbFrame *source, unsigned x, unsigned y,
                                 unsigned size, int use_v);

/* ------------------------------------------------------------------------- *
 * Shared frame encoder.
 *
 * The block grammar, motion search, quality model, copy reconstruction and the
 * data-vs-split size decision are identical across the Moving Blocks family;
 * only the data-block coding (luma residuals and predictor) differs. mb_encode_
 * frame implements everything shared and reaches the codec-specific part through
 * MbEncodeDataCodec, so each codec's public encode_frame is a thin adapter.
 * ------------------------------------------------------------------------- */

/* Copy-family selection policy. */
typedef enum {
    /* Historical order: take the first accepted stationary, temporal, spatial. */
    MB_ENCODE_POLICY_ORDERED,
    /* Compare every accepted copy by error, then encoded bits, then order. */
    MB_ENCODE_POLICY_LOWEST_ERROR
} MbEncodePolicy;

/* Per-frame block tally and search-evaluation counters. */
typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
    size_t spatial4x4_blocks;
    size_t split4x4_blocks;
    size_t data2x2_blocks;
    size_t stationary2x2_blocks;
    size_t temporal2x2_blocks;
    size_t spatial2x2_blocks;
    size_t stationary4x4_evaluations;
    size_t temporal4x4_evaluations;
    size_t spatial4x4_evaluations;
    size_t stationary2x2_evaluations;
    size_t temporal2x2_evaluations;
    size_t spatial2x2_evaluations;
    size_t bits_written;
} MbEncodeStats;

/*
 * Codec data-block coding. encode_data writes one size x size data block to
 * `writer` from `source` at (x, y), reconstructs it into `recon` (at
 * recon_stride, a compact 4-pitch buffer during candidate evaluation), and
 * advances `predictor`. Luma is lossless in every family codec, so the
 * reconstruction is the source luma plus the block-average chroma.
 */
typedef struct {
    ReplayStatus (*encode_data)(ReplayBitWriter *writer, const MbFrame *source,
                                unsigned x, unsigned y, unsigned size,
                                MbPixel *recon, size_t recon_stride,
                                MbPredictor *predictor);
} MbEncodeDataCodec;

/* Enabled block decisions and quality/rate state for one frame. */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    unsigned loss_level;
    MbEncodePolicy policy;
    MbEncodeWorkspace *workspace; /* optional temporal-search cache */
} MbEncodeOptions;

/*
 * Choose the copy family for one 4x4 block (or 2x2 split child). The search and
 * scoring are grammar-independent -- the returned CopyCandidate names the mode
 * (data, stationary, temporal, spatial) and, for copies, the vector -- so the
 * per-codec frame encoders share these regardless of how they emit the choice.
 * `quality` is the codec-private threshold pointer forwarded to block_match.
 */
CopyCandidate mb_encode_select_copy4x4(
    const MbEncodeCodec *enc, const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, unsigned x, unsigned y, uint8_t u, uint8_t v,
    int allow_stationary, int allow_temporal, int allow_spatial,
    const void *quality, unsigned loss_level, MbEncodePolicy policy,
    MbEncodeStats *stats, MbEncodeWorkspace *workspace);

CopyCandidate mb_encode_select_copy2x2(
    const MbEncodeCodec *enc, const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned parent_x, unsigned parent_y, unsigned x,
    unsigned y, uint8_t u, uint8_t v, int allow_stationary, int allow_temporal,
    int allow_spatial, const void *quality, unsigned loss_level,
    MbEncodePolicy policy, MbEncodeStats *stats, MbEncodeWorkspace *workspace);

/*
 * Encode one frame. `search` supplies the motion tables and quality model,
 * `data` the codec's data-block coding. `source` holds quantised working pixels;
 * `reconstructed` is filled with exactly what a decoder retains and is the
 * `previous` for the next inter frame. `output` is cleared on entry. The caller
 * is responsible for codec-specific source-range validation. `stats` optional.
 */
ReplayStatus mb_encode_frame(const MbEncodeCodec *search,
                             const MbEncodeDataCodec *data,
                             const MbFrame *source, const MbFrame *previous,
                             const MbEncodeOptions *options,
                             ReplayBuffer *output, MbFrame *reconstructed,
                             MbEncodeStats *stats);

#endif
