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
