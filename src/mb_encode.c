#include "replay/mb_encode.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "replay/mb_quality.h"

void mb_encode_copy_motion(const MbFrame *reference, MbFrame *destination,
                           unsigned x, unsigned y, unsigned size,
                           const MbMotionVector *motion)
{
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);
    unsigned row;

    for (row = 0U; row < size; ++row) {
        unsigned column;
        for (column = 0U; column < size; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] =
                reference->pixels[(size_t)(source_y + row) * reference->stride +
                                  source_x + column];
        }
    }
}

void mb_encode_fill_tentative_copy2x2(
    const MbFrame *reference, const MbFrame *reconstructed,
    MbPixel tentative[16], unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, const MbMotionVector *motion,
    unsigned *available_mask)
{
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);
    unsigned row;

    for (row = 0U; row < 2U; ++row) {
        unsigned column;
        for (column = 0U; column < 2U; ++column) {
            unsigned local = (y + row - parent_y) * 4U + x + column - parent_x;
            const MbPixel *pixel;

            if (motion->spatial != 0) {
                pixel = mb_encode_split_spatial_pixel(
                    reconstructed, tentative, *available_mask,
                    parent_x, parent_y, source_x + column, source_y + row);
            } else {
                pixel = &reference->pixels[(size_t)(source_y + row) *
                                               reference->stride +
                                           source_x + column];
            }
            tentative[local] = *pixel;
            *available_mask |= 1U << local;
        }
    }
}

ReplayStatus mb_encode_append_candidate(ReplayBitWriter *writer,
                                        const ReplayBuffer *buffer, size_t bits)
{
    ReplayBitReader reader;

    replay_bitreader_init(&reader, buffer->data, buffer->size);
    while (bits != 0U) {
        unsigned count = bits > 32U ? 32U : (unsigned)bits;
        uint32_t value;
        ReplayStatus status = replay_bitreader_read(&reader, count, &value);

        if (status != REPLAY_OK) {
            return status;
        }
        status = replay_bitwriter_write(writer, value, count);
        if (status != REPLAY_OK) {
            return status;
        }
        bits -= count;
    }
    return REPLAY_OK;
}

unsigned mb_encode_motion_bits(const MbMotionVector *motion,
                               MbMotionBlockSize block_size)
{
    unsigned magnitude;
    unsigned body_bits;

    if (motion->spatial != 0) {
        body_bits = 7U;
    } else {
        magnitude = (unsigned)(motion->dx < 0 ? -motion->dx : motion->dx);
        if ((unsigned)(motion->dy < 0 ? -motion->dy : motion->dy) > magnitude) {
            magnitude = (unsigned)(motion->dy < 0 ? -motion->dy : motion->dy);
        }
        body_bits = magnitude <= 1U ? 5U
                  : magnitude == 2U ? 6U
                  : magnitude == 3U ? 7U
                                    : 10U;
    }
    return body_bits + (block_size == MB_MOTION_BLOCK_4X4 ? 2U : 1U);
}

unsigned mb_encode_motion_bits_format7(const MbMotionVector *motion,
                                       MbMotionBlockSize block_size)
{
    unsigned magnitude;
    unsigned body_bits;

    if (motion->spatial != 0) {
        body_bits = 8U; /* `11` family + 6-bit spatial index */
    } else {
        magnitude = (unsigned)(motion->dx < 0 ? -motion->dx : motion->dx);
        if ((unsigned)(motion->dy < 0 ? -motion->dy : motion->dy) > magnitude) {
            magnitude = (unsigned)(motion->dy < 0 ? -motion->dy : motion->dy);
        }
        body_bits = magnitude == 0U ? 2U  /* `00` stationary */
                  : magnitude == 1U ? 5U  /* `01` + 3 */
                  : magnitude == 2U ? 6U  /* `10` + 4 */
                                    : 8U; /* `11` + 6 (radius 3 or 4) */
    }
    /* The move opcode is the same width as 17/19's: 2 bits 4x4, 1 bit 2x2. */
    return body_bits + (block_size == MB_MOTION_BLOCK_4X4 ? 2U : 1U);
}

int mb_encode_candidate_better(const CopyCandidate *candidate,
                               const CopyCandidate *best)
{
    return candidate->valid &&
           (!best->valid || candidate->error < best->error ||
            (candidate->error == best->error &&
             (candidate->bits < best->bits ||
              (candidate->bits == best->bits &&
               candidate->order < best->order))));
}

const MbPixel *mb_encode_split_spatial_pixel(
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned block_x, unsigned block_y,
    unsigned source_x, unsigned source_y)
{
    /*
     * A spatial source can fall inside the 4x4 block currently being tested.
     * In that case it is legal only if an earlier 2x2 decision has already
     * produced the pixel in the private tentative reconstruction.
     */
    if (source_x >= block_x && source_x < block_x + 4U &&
        source_y >= block_y && source_y < block_y + 4U) {
        unsigned local = (source_y - block_y) * 4U + source_x - block_x;
        return (available_mask & (1U << local)) != 0U
                   ? &tentative[local]
                   : NULL;
    }

    /*
     * Top-level blocks are reconstructed as complete 4x4 units in raster order.
     * A 2x2 vector can point upward and right; near the right edge of the
     * current parent that rectangle may cross into the next 4x4 block, whose
     * pixels do not exist yet. Pixels outside the tentative parent are available
     * only when their owning top-level block precedes the current parent in
     * codec scan order.
     */
    {
        unsigned source_block_x = source_x & ~3U;
        unsigned source_block_y = source_y & ~3U;

        if (source_block_y > block_y ||
            (source_block_y == block_y && source_block_x >= block_x)) {
            return NULL;
        }
    }
    return &reconstructed->pixels[(size_t)source_y * reconstructed->stride +
                                  source_x];
}

ReplayStatus mb_encode_workspace_init(MbEncodeWorkspace *workspace,
                                      unsigned width, unsigned height)
{
    size_t count4x4;
    size_t count2x2;

    if (workspace == NULL || width == 0U || height == 0U ||
        (width & 3U) != 0U || (height & 3U) != 0U ||
        (size_t)(width / 4U) > SIZE_MAX / (height / 4U) ||
        (size_t)(width / 2U) > SIZE_MAX / (height / 2U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    count4x4 = (size_t)(width / 4U) * (height / 4U);
    count2x2 = (size_t)(width / 2U) * (height / 2U);
    workspace->entries4x4 = calloc(count4x4, sizeof(*workspace->entries4x4));
    workspace->entries2x2 = calloc(count2x2, sizeof(*workspace->entries2x2));
    if (workspace->entries4x4 == NULL || workspace->entries2x2 == NULL) {
        free(workspace->entries2x2);
        free(workspace->entries4x4);
        workspace->entries4x4 = NULL;
        workspace->entries2x2 = NULL;
        return REPLAY_OUT_OF_MEMORY;
    }
    workspace->width = width;
    workspace->height = height;
    workspace->count4x4 = count4x4;
    workspace->count2x2 = count2x2;
    return REPLAY_OK;
}

void mb_encode_workspace_reset(MbEncodeWorkspace *workspace)
{
    if (workspace != NULL && workspace->entries4x4 != NULL) {
        memset(workspace->entries4x4, 0,
               workspace->count4x4 * sizeof(*workspace->entries4x4));
        memset(workspace->entries2x2, 0,
               workspace->count2x2 * sizeof(*workspace->entries2x2));
    }
}

void mb_encode_workspace_destroy(MbEncodeWorkspace *workspace)
{
    if (workspace != NULL) {
        free(workspace->entries2x2);
        free(workspace->entries4x4);
        memset(workspace, 0, sizeof(*workspace));
    }
}

/*
 * Fixed-loss temporal search used when there is no rate-control cache to fill.
 * Instead of profiling every candidate at every quality level it scores each at
 * the single target level with partial-distortion early-out (mb_quality_match_
 * pruned), giving the identical lowest-error/enumeration-order winner far faster
 * -- most candidates abort after a row or two once a good match is found. Same
 * result as the cached path at `loss_level`, just without the per-level work.
 */
static int temporal_search_pruned(const MbEncodeCodec *enc,
                                  const MbFrame *source, const MbFrame *previous,
                                  unsigned x, unsigned y, unsigned size,
                                  uint8_t u, uint8_t v, unsigned loss_level,
                                  MbMotionVector *motion,
                                  unsigned *matched_error, size_t *evaluations)
{
    MbQualityThresholds thresholds;
    unsigned best_error = UINT_MAX;
    MbMotionVector best = { 0, 0, 0 };
    int found = 0;
    unsigned index;

    if (mb_quality_thresholds(loss_level, &thresholds) != REPLAY_OK) {
        return 0;
    }
    for (index = 0U; index < enc->temporal_count; ++index) {
        MbMotionVector candidate;
        int source_x;
        int source_y;
        unsigned err;

        if (!enc->temporal_vector(index, &candidate)) {
            break;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + (int)size > (int)previous->width ||
            source_y + (int)size > (int)previous->height) {
            continue;
        }
        ++*evaluations;
        if (mb_quality_match_pruned(source, x, y, previous, (unsigned)source_x,
                                    (unsigned)source_y, size, u, v,
                                    enc->chroma_half, &thresholds, best_error,
                                    &err) &&
            (!found || err < best_error)) {
            best = candidate;
            best_error = err;
            found = 1;
            if (err == 0U) {
                break;
            }
        }
    }
    if (!found) {
        return 0;
    }
    *motion = (MbMotionVector){ best.dx, best.dy, 0 };
    *matched_error = best_error;
    return 1;
}

/*
 * Search the temporal vector sequence and compare 2x2 reconstruction targets.
 * Lowest error wins; enumeration order yields the shortest/source-first code
 * for equal-error candidates. Results cache per quality level so repeated rate
 * retries reuse the work.
 */
int mb_encode_match_temporal2x2(const MbEncodeCodec *enc, const MbFrame *source,
                                const MbFrame *previous, unsigned x, unsigned y,
                                uint8_t u, uint8_t v, unsigned loss_level,
                                MbMotionVector *motion, unsigned *matched_error,
                                size_t *evaluations,
                                MbEncodeWorkspace *workspace)
{
    MbEncodeTemporalEntry local_entry = { { { 0 } }, 0U };
    MbEncodeTemporalEntry *entry = &local_entry;
    unsigned index;

    if (workspace != NULL) {
        size_t cache_index = (size_t)(y / 2U) * (workspace->width / 2U) +
                             x / 2U;
        entry = &workspace->entries2x2[cache_index];
    }
    if (entry->ready) {
        const MbEncodeTemporalResult *result = &entry->levels[loss_level];
        if (!result->valid) {
            return 0;
        }
        *motion = (MbMotionVector){ result->dx, result->dy, 0 };
        *matched_error = result->error;
        return 1;
    }
    if (workspace == NULL) {
        return temporal_search_pruned(enc, source, previous, x, y, 2U, u, v,
                                      loss_level, motion, matched_error,
                                      evaluations);
    }

    for (index = 0U; index < enc->temporal_count; ++index) {
        MbMotionVector candidate;
        unsigned total_error;
        int source_x;
        int source_y;
        unsigned level;

        if (!enc->temporal_vector(index, &candidate)) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 2 > (int)previous->width ||
            source_y + 2 > (int)previous->height) {
            continue;
        }
        ++*evaluations;
        if (!enc->profile_match(
                source, x, y, previous, (unsigned)source_x,
                (unsigned)source_y, 2U, u, v, &total_error, &level)) {
            continue;
        }
        for (; level < enc->level_count; ++level) {
            MbEncodeTemporalResult *result = &entry->levels[level];

            if (!result->valid || total_error < result->error) {
                result->dx = (int8_t)candidate.dx;
                result->dy = (int8_t)candidate.dy;
                result->error = (uint16_t)total_error;
                result->valid = 1U;
            }
        }
        /* Zero error is optimal at every quality row and table-order tie. */
        if (total_error == 0U) {
            break;
        }
    }
    entry->ready = 1U;
    if (!entry->levels[loss_level].valid) {
        return 0;
    }
    *motion = (MbMotionVector){ entry->levels[loss_level].dx,
                               entry->levels[loss_level].dy, 0 };
    *matched_error = entry->levels[loss_level].error;
    return 1;
}

int mb_encode_find_temporal4x4(const MbEncodeCodec *enc, const MbFrame *source,
                               const MbFrame *previous, unsigned x, unsigned y,
                               uint8_t u, uint8_t v, unsigned loss_level,
                               MbMotionVector *motion, unsigned *matched_error,
                               size_t *evaluations, MbEncodeWorkspace *workspace)
{
    MbEncodeTemporalEntry local_entry = { { { 0 } }, 0U };
    MbEncodeTemporalEntry *entry = &local_entry;
    unsigned index;

    if (workspace != NULL) {
        size_t cache_index = (size_t)(y / 4U) * (workspace->width / 4U) +
                             x / 4U;
        entry = &workspace->entries4x4[cache_index];
    }
    if (entry->ready) {
        const MbEncodeTemporalResult *result = &entry->levels[loss_level];
        if (!result->valid) {
            return 0;
        }
        *motion = (MbMotionVector){ result->dx, result->dy, 0 };
        *matched_error = result->error;
        return 1;
    }
    if (workspace == NULL) {
        return temporal_search_pruned(enc, source, previous, x, y, 4U, u, v,
                                      loss_level, motion, matched_error,
                                      evaluations);
    }

    /* Enumeration order gives the shortest/source-first equal-error code. */
    for (index = 0U; index < enc->temporal_count; ++index) {
        MbMotionVector candidate;
        unsigned total_error;
        int source_x;
        int source_y;
        unsigned level;

        if (!enc->temporal_vector(index, &candidate)) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 4 > (int)previous->width ||
            source_y + 4 > (int)previous->height) {
            continue;
        }
        ++*evaluations;
        if (!enc->profile_match(
                source, x, y, previous, (unsigned)source_x,
                (unsigned)source_y, 4U, u, v, &total_error, &level)) {
            continue;
        }
        for (; level < enc->level_count; ++level) {
            MbEncodeTemporalResult *result = &entry->levels[level];

            if (!result->valid || total_error < result->error) {
                result->dx = (int8_t)candidate.dx;
                result->dy = (int8_t)candidate.dy;
                result->error = (uint16_t)total_error;
                result->valid = 1U;
            }
        }
        /* Zero error is optimal at every quality row and table-order tie. */
        if (total_error == 0U) {
            break;
        }
    }
    entry->ready = 1U;
    if (!entry->levels[loss_level].valid) {
        return 0;
    }
    *motion = (MbMotionVector){ entry->levels[loss_level].dx,
                               entry->levels[loss_level].dy, 0 };
    *matched_error = entry->levels[loss_level].error;
    return 1;
}

static int spatial_block_matches(const MbEncodeCodec *enc,
                                 const MbFrame *source,
                                 const MbFrame *reconstructed, unsigned x,
                                 unsigned y, const MbMotionVector *motion,
                                 uint8_t u, uint8_t v, const void *quality,
                                 unsigned *error)
{
    int source_x = (int)x + motion->dx;
    int source_y = (int)y + motion->dy;

    /* Legal table entries can still be out of bounds near frame edges. */
    if (source_x < 0 || source_y < 0 ||
        source_x + 4 > (int)reconstructed->width ||
        source_y + 4 > (int)reconstructed->height) {
        return 0;
    }
    return enc->block_match(source, x, y, reconstructed, (unsigned)source_x,
                            (unsigned)source_y, 4U, u, v, quality, error);
}

int mb_encode_find_spatial4x4(const MbEncodeCodec *enc, const MbFrame *source,
                              const MbFrame *reconstructed, unsigned x,
                              unsigned y, uint8_t u, uint8_t v,
                              const void *quality, MbMotionVector *motion,
                              unsigned *matched_error, size_t *evaluations)
{
    unsigned best_error = UINT32_MAX;
    unsigned index;

    /* Minimise distortion; source table order breaks equal-error ties. */
    for (index = 0U; index < enc->spatial_count; ++index) {
        MbMotionVector candidate;
        unsigned error;

        if (!enc->spatial_vector(MB_MOTION_BLOCK_4X4, index, &candidate)) {
            return 0;
        }
        {
            int source_x = (int)x + candidate.dx;
            int source_y = (int)y + candidate.dy;
            if (source_x >= 0 && source_y >= 0 &&
                source_x + 4 <= (int)reconstructed->width &&
                source_y + 4 <= (int)reconstructed->height) {
                ++*evaluations;
            }
        }
        if (spatial_block_matches(enc, source, reconstructed, x, y, &candidate,
                                  u, v, quality, &error) &&
            error < best_error) {
            *motion = candidate;
            best_error = error;
            if (error == 0U) {
                break;
            }
        }
    }
    if (best_error == UINT32_MAX) {
        return 0;
    }
    *matched_error = best_error;
    return 1;
}

int mb_encode_match_spatial2x2(const MbEncodeCodec *enc, const MbFrame *source,
                               const MbFrame *reconstructed,
                               const MbPixel tentative[16],
                               unsigned available_mask, unsigned parent_x,
                               unsigned parent_y, unsigned x, unsigned y,
                               uint8_t u, uint8_t v, const void *quality,
                               MbMotionVector *motion, unsigned *matched_error,
                               size_t *evaluations)
{
    unsigned best_error = UINT32_MAX;
    unsigned index;

    for (index = 0U; index < enc->spatial_count; ++index) {
        MbMotionVector candidate;
        MbPixel candidate_pixels[4];
        MbFrame candidate_frame = { 2U, 2U, 2U, candidate_pixels };
        int source_x;
        int source_y;
        unsigned row;
        unsigned error;

        if (!enc->spatial_vector(MB_MOTION_BLOCK_2X2, index, &candidate)) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 2 > (int)reconstructed->width ||
            source_y + 2 > (int)reconstructed->height) {
            continue;
        }
        for (row = 0U; row < 2U; ++row) {
            unsigned column;
            for (column = 0U; column < 2U; ++column) {
                const MbPixel *reference = mb_encode_split_spatial_pixel(
                    reconstructed, tentative, available_mask,
                    parent_x, parent_y, (unsigned)source_x + column,
                    (unsigned)source_y + row);
                if (reference == NULL) {
                    break;
                }
                candidate_pixels[row * 2U + column] = *reference;
            }
            if (column != 2U) {
                break;
            }
        }
        if (row == 2U) {
            ++*evaluations;
        }
        if (row == 2U && enc->block_match(
                             source, x, y, &candidate_frame, 0U, 0U, 2U,
                             u, v, quality, &error) &&
            error < best_error) {
            *motion = candidate;
            best_error = error;
            /* All spatial codes have equal length; table order breaks ties. */
            if (error == 0U) {
                break;
            }
        }
    }
    if (best_error == UINT32_MAX) {
        return 0;
    }
    *matched_error = best_error;
    return 1;
}

uint8_t mb_encode_average_chroma(const MbFrame *source, unsigned x, unsigned y,
                                 unsigned size, int use_v, int chroma_half)
{
    int sum = 0;
    int area = (int)(size * size);
    unsigned row;

    for (row = 0U; row < size; ++row) {
        unsigned column;
        for (column = 0U; column < size; ++column) {
            const MbPixel *pixel =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            int value = use_v != 0 ? pixel->v : pixel->u;

            /* Two's-complement modulo 2*chroma_half (16 for 5-bit, 32 6-bit). */
            sum += value < chroma_half ? value : value - 2 * chroma_half;
        }
    }
    /* Match the source assembler's arithmetic shift, including negatives. */
    if (sum < 0) {
        sum = -((-sum + area - 1) / area);
    } else {
        sum /= area;
    }
    return (uint8_t)(sum & (2 * chroma_half - 1));
}

/*
 * Choose a top-level 4x4 block's copy mode. Under the ordered policy the first
 * accepted family wins (stationary, temporal, spatial); under lowest-error every
 * accepted family competes by error, bits, then a stable order. The stationary
 * test reuses the codec's block_match against the data-block average (u, v).
 */
CopyCandidate mb_encode_select_copy4x4(
    const MbEncodeCodec *enc, const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, unsigned x, unsigned y, uint8_t u, uint8_t v,
    int allow_stationary, int allow_temporal, int allow_spatial,
    const void *quality, unsigned loss_level, MbEncodePolicy policy,
    MbEncodeStats *stats, MbEncodeWorkspace *workspace)
{
    CopyCandidate selected = { SPLIT_MODE_DATA, { 0, 0, 0 }, 0U, 0U, 0U, 0 };
    CopyCandidate candidate;
    unsigned error;

    if (allow_stationary) {
        ++stats->stationary4x4_evaluations;
    }
    if (allow_stationary &&
        enc->block_match(source, x, y, previous, x, y, 4U, u, v, quality,
                         &error)) {
        selected = (CopyCandidate){
            SPLIT_MODE_STATIONARY, { 0, 0, 0 }, error, 2U, 0U, 1
        };
        if (policy == MB_ENCODE_POLICY_ORDERED || error == 0U) {
            return selected;
        }
    }

    candidate = (CopyCandidate){ SPLIT_MODE_TEMPORAL, { 0, 0, 0 }, 0U, 0U, 1U, 0 };
    if (allow_temporal &&
        mb_encode_find_temporal4x4(enc, source, previous, x, y, u, v,
                                   loss_level, &candidate.motion, &error,
                                   &stats->temporal4x4_evaluations, workspace)) {
        candidate.error = error;
        candidate.bits = enc->motion_bits(&candidate.motion,
                                               MB_MOTION_BLOCK_4X4);
        candidate.valid = 1;
        if (policy == MB_ENCODE_POLICY_ORDERED) {
            return candidate;
        }
        if (mb_encode_candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }

    candidate = (CopyCandidate){ SPLIT_MODE_SPATIAL, { 0, 0, 1 }, 0U, 0U, 2U, 0 };
    if (allow_spatial &&
        mb_encode_find_spatial4x4(enc, source, reconstructed, x, y, u, v,
                                  quality, &candidate.motion, &error,
                                  &stats->spatial4x4_evaluations)) {
        candidate.error = error;
        candidate.bits = enc->motion_bits(&candidate.motion,
                                               MB_MOTION_BLOCK_4X4);
        candidate.valid = 1;
        if (mb_encode_candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }
    return selected;
}

/* The 2x2 split-child analogue of select_copy4x4, scoring spatial copies
 * against the parent's tentative reconstruction. */
CopyCandidate mb_encode_select_copy2x2(
    const MbEncodeCodec *enc, const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, uint8_t u, uint8_t v, int allow_stationary,
    int allow_temporal, int allow_spatial, const void *quality,
    unsigned loss_level, MbEncodePolicy policy, MbEncodeStats *stats,
    MbEncodeWorkspace *workspace)
{
    CopyCandidate selected = { SPLIT_MODE_DATA, { 0, 0, 0 }, 0U, 0U, 0U, 0 };
    CopyCandidate candidate;
    unsigned error;

    if (allow_stationary) {
        ++stats->stationary2x2_evaluations;
    }
    if (allow_stationary &&
        enc->block_match(source, x, y, previous, x, y, 2U, u, v, quality,
                         &error)) {
        selected = (CopyCandidate){
            SPLIT_MODE_STATIONARY, { 0, 0, 0 }, error, 2U, 0U, 1
        };
        if (policy == MB_ENCODE_POLICY_ORDERED || error == 0U) {
            return selected;
        }
    }

    candidate = (CopyCandidate){ SPLIT_MODE_TEMPORAL, { 0, 0, 0 }, 0U, 0U, 1U, 0 };
    if (allow_temporal &&
        mb_encode_match_temporal2x2(enc, source, previous, x, y, u, v,
                                    loss_level, &candidate.motion, &error,
                                    &stats->temporal2x2_evaluations,
                                    workspace)) {
        candidate.error = error;
        candidate.bits = enc->motion_bits(&candidate.motion,
                                               MB_MOTION_BLOCK_2X2);
        candidate.valid = 1;
        if (policy == MB_ENCODE_POLICY_ORDERED) {
            return candidate;
        }
        if (mb_encode_candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }

    candidate = (CopyCandidate){ SPLIT_MODE_SPATIAL, { 0, 0, 1 }, 0U, 0U, 2U, 0 };
    if (allow_spatial &&
        mb_encode_match_spatial2x2(enc, source, reconstructed, tentative,
                                   available_mask, parent_x, parent_y, x, y,
                                   u, v, quality, &candidate.motion, &error,
                                   &stats->spatial2x2_evaluations)) {
        candidate.error = error;
        candidate.bits = enc->motion_bits(&candidate.motion,
                                               MB_MOTION_BLOCK_2X2);
        candidate.valid = 1;
        if (mb_encode_candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }
    return selected;
}

/* Encode a data 4x4 to a scratch buffer, reconstructing into a compact block. */
static ReplayStatus build_data_candidate(
    const MbEncodeDataCodec *data, const MbFrame *source, unsigned x,
    unsigned y, const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPixel recon[16], MbPredictor *final_predictor, size_t *bits)
{
    ReplayBitWriter writer;
    ReplayStatus status;

    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    *final_predictor = *initial_predictor;
    status = data->encode_data(&writer, source, x, y, 4U, recon, 4U,
                               final_predictor);
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Encode the four 2x2 split children to a scratch buffer, building the parent's
 * tentative reconstruction and reporting each child's chosen mode. */
static ReplayStatus build_split_candidate(
    const MbEncodeCodec *enc, const MbEncodeDataCodec *data,
    const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, unsigned x, unsigned y, int allow_stationary,
    int allow_temporal, int allow_spatial, const void *quality,
    unsigned loss_level, MbEncodePolicy policy,
    const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPredictor *final_predictor, MbPixel tentative[16], SplitMode modes[4],
    size_t *bits, MbEncodeStats *stats, MbEncodeWorkspace *workspace)
{
    /* Decoder order inside a split block is TL, TR, BL, BR. */
    static const unsigned offsets[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    ReplayBitWriter writer;
    unsigned available_mask = 0U;
    unsigned block;
    ReplayStatus status;

    for (block = 0U; block < 16U; ++block) {
        tentative[block] = (MbPixel){ 0U, 0U, 0U };
    }
    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    *final_predictor = *initial_predictor;
    status = replay_bitwriter_write(&writer, UINT32_C(3), 2U);
    for (block = 0U; status == REPLAY_OK && block < 4U; ++block) {
        unsigned block_x = x + offsets[block][0];
        unsigned block_y = y + offsets[block][1];
        unsigned local = (block_y - y) * 4U + block_x - x;
        uint8_t u = mb_encode_average_chroma(source, block_x, block_y, 2U, 0,
                                             enc->chroma_half);
        uint8_t v = mb_encode_average_chroma(source, block_x, block_y, 2U, 1,
                                             enc->chroma_half);
        CopyCandidate selected = mb_encode_select_copy2x2(
            enc, source, previous, reconstructed, tentative, available_mask,
            x, y, block_x, block_y, u, v, allow_stationary, allow_temporal,
            allow_spatial, quality, loss_level, policy, stats, workspace);

        modes[block] = selected.mode;
        if (selected.mode == SPLIT_MODE_STATIONARY) {
            status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
            mb_encode_fill_tentative_copy2x2(previous, reconstructed, tentative,
                                             x, y, block_x, block_y,
                                             &selected.motion, &available_mask);
        } else if (selected.mode == SPLIT_MODE_TEMPORAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(&writer, MB_MOTION_BLOCK_2X2,
                                                  &selected.motion);
            }
            mb_encode_fill_tentative_copy2x2(previous, reconstructed, tentative,
                                             x, y, block_x, block_y,
                                             &selected.motion, &available_mask);
        } else if (selected.mode == SPLIT_MODE_SPATIAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(&writer, MB_MOTION_BLOCK_2X2,
                                                  &selected.motion);
            }
            mb_encode_fill_tentative_copy2x2(reconstructed, reconstructed,
                                             tentative, x, y, block_x, block_y,
                                             &selected.motion, &available_mask);
        } else {
            status = data->encode_data(&writer, source, block_x, block_y, 2U,
                                       &tentative[local], 4U, final_predictor);
            available_mask |= 1U << local;
            available_mask |= 1U << (local + 1U);
            available_mask |= 1U << (local + 4U);
            available_mask |= 1U << (local + 5U);
        }
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Write a reconstructed 4x4 block (compact 16-pixel layout) into the frame. */
static void blit_block4x4(MbFrame *destination, unsigned x, unsigned y,
                          const MbPixel block[16])
{
    unsigned row;

    for (row = 0U; row < 4U; ++row) {
        unsigned column;
        for (column = 0U; column < 4U; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] = block[row * 4U + column];
        }
    }
}

static int valid_frame_pair(const MbFrame *source, const MbFrame *reconstructed)
{
    return source != NULL && source->pixels != NULL &&
           reconstructed != NULL && reconstructed->pixels != NULL &&
           source->width != 0U && source->height != 0U &&
           (source->width & 3U) == 0U && (source->height & 3U) == 0U &&
           source->stride >= source->width &&
           reconstructed->width == source->width &&
           reconstructed->height == source->height &&
           reconstructed->stride >= reconstructed->width;
}

ReplayStatus mb_encode_frame(const MbEncodeCodec *search,
                             const MbEncodeDataCodec *data,
                             const MbFrame *source, const MbFrame *previous,
                             const MbEncodeOptions *options,
                             ReplayBuffer *output, MbFrame *reconstructed,
                             MbEncodeStats *stats)
{
    ReplayBitWriter writer;
    ReplayBuffer data_candidate;
    ReplayBuffer split_candidate;
    /* Predictor lifetime is one frame; its specified initial value is zero. */
    MbPredictor predictor = { 0 };
    MbEncodeStats local_stats = { 0 };
    MbQualityThresholds quality;
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    MbEncodePolicy policy;
    MbEncodeWorkspace *workspace;
    unsigned loss_level;
    unsigned y;
    ReplayStatus status = REPLAY_OK;

    if (search == NULL || data == NULL || options == NULL || output == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    allow_stationary = options->allow_stationary != 0;
    allow_temporal = options->allow_temporal != 0;
    allow_spatial = options->allow_spatial != 0;
    allow_split = options->allow_split != 0;
    policy = options->policy;
    workspace = options->workspace;
    loss_level = options->loss_level;

    replay_buffer_clear(output);
    if ((policy != MB_ENCODE_POLICY_ORDERED &&
         policy != MB_ENCODE_POLICY_LOWEST_ERROR) ||
        mb_quality_thresholds(loss_level, &quality) != REPLAY_OK ||
        !valid_frame_pair(source, reconstructed) ||
        (workspace != NULL &&
         (workspace->width != source->width ||
          workspace->height != source->height)) ||
        ((allow_stationary || allow_temporal) &&
         (previous == NULL || previous->pixels == NULL ||
          previous->width != source->width ||
          previous->height != source->height ||
          previous->stride < previous->width))) {
        return REPLAY_INVALID_ARGUMENT;
    }

    replay_bitwriter_init(&writer, output);
    /* Reused scratch buffers avoid reallocating for every data/split decision. */
    replay_buffer_init(&data_candidate);
    replay_buffer_init(&split_candidate);
    /* Raster order is part of the format, not an optimisation choice. */
    for (y = 0; status == REPLAY_OK && y < source->height; y += 4U) {
        unsigned x;
        for (x = 0; status == REPLAY_OK && x < source->width; x += 4U) {
            uint8_t u = mb_encode_average_chroma(source, x, y, 4U, 0,
                                                 search->chroma_half);
            uint8_t v = mb_encode_average_chroma(source, x, y, 4U, 1,
                                                 search->chroma_half);
            CopyCandidate copy = mb_encode_select_copy4x4(
                search, source, previous, reconstructed, x, y, u, v,
                allow_stationary, allow_temporal, allow_spatial, &quality,
                loss_level, policy, &local_stats, workspace);

            if (copy.mode == SPLIT_MODE_STATIONARY) {
                status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
                mb_encode_copy_motion(previous, reconstructed, x, y, 4U,
                                      &(MbMotionVector){ 0, 0, 0 });
                ++local_stats.stationary4x4_blocks;
            } else if (copy.mode == SPLIT_MODE_TEMPORAL) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(&writer,
                                                      MB_MOTION_BLOCK_4X4,
                                                      &copy.motion);
                }
                mb_encode_copy_motion(previous, reconstructed, x, y, 4U,
                                      &copy.motion);
                ++local_stats.temporal4x4_blocks;
            } else if (copy.mode == SPLIT_MODE_SPATIAL) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(&writer,
                                                      MB_MOTION_BLOCK_4X4,
                                                      &copy.motion);
                }
                mb_encode_copy_motion(reconstructed, reconstructed, x, y, 4U,
                                      &copy.motion);
                ++local_stats.spatial4x4_blocks;
            } else {
                /* No accepted copy: weigh a data 4x4 against a 2x2 split and
                   keep whichever encodes smaller (ties stay 4x4). */
                MbPixel data_recon[16];
                MbPixel split_recon[16];
                MbPredictor data_predictor;
                MbPredictor split_predictor;
                SplitMode split_modes[4];
                size_t data_bits;
                size_t split_bits = SIZE_MAX;

                status = build_data_candidate(data, source, x, y, &predictor,
                                              &data_candidate, data_recon,
                                              &data_predictor, &data_bits);
                if (status == REPLAY_OK && allow_split) {
                    status = build_split_candidate(
                        search, data, source, previous, reconstructed, x, y,
                        allow_stationary, allow_temporal, allow_spatial,
                        &quality, loss_level, policy, &predictor,
                        &split_candidate, &split_predictor, split_recon,
                        split_modes, &split_bits, &local_stats, workspace);
                }
                if (status == REPLAY_OK && split_bits < data_bits) {
                    unsigned block;

                    status = mb_encode_append_candidate(&writer,
                                                        &split_candidate,
                                                        split_bits);
                    predictor = split_predictor;
                    blit_block4x4(reconstructed, x, y, split_recon);
                    for (block = 0U; block < 4U; ++block) {
                        switch (split_modes[block]) {
                        case SPLIT_MODE_STATIONARY:
                            ++local_stats.stationary2x2_blocks;
                            break;
                        case SPLIT_MODE_TEMPORAL:
                            ++local_stats.temporal2x2_blocks;
                            break;
                        case SPLIT_MODE_SPATIAL:
                            ++local_stats.spatial2x2_blocks;
                            break;
                        default:
                            ++local_stats.data2x2_blocks;
                            break;
                        }
                    }
                    ++local_stats.split4x4_blocks;
                } else if (status == REPLAY_OK) {
                    status = mb_encode_append_candidate(&writer,
                                                        &data_candidate,
                                                        data_bits);
                    predictor = data_predictor;
                    blit_block4x4(reconstructed, x, y, data_recon);
                    ++local_stats.data4x4_blocks;
                }
            }
        }
    }

    if (status == REPLAY_OK) {
        local_stats.bits_written = replay_bitwriter_position(&writer);
        status = replay_bitwriter_flush_zero(&writer);
    }
    replay_buffer_free(&split_candidate);
    replay_buffer_free(&data_candidate);
    if (status != REPLAY_OK) {
        replay_buffer_clear(output);
        return status;
    }
    if (stats != NULL) {
        *stats = local_stats;
    }
    return REPLAY_OK;
}
