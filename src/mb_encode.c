#include "replay/mb_encode.h"

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
