#include "replay/mb_encode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
