#ifndef REPLAY_TYPE2_H
#define REPLAY_TYPE2_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_ae7.h"

/* Return the number of fixed-size 16-bit frames described by the catalogue. */
ReplayStatus replay_type2_frame_count(const ReplayAe7Movie *movie,
                                      size_t *frame_count);

/*
 * Extract one type 2 frame using the field interpretation historically seen
 * by the type 19 compressor: Y=word[5:0], U=word[10:6], V=word[15:11].
 * This deliberately reinterprets the stored halfword; it is not a YUV555
 * colour-space conversion.
 */
ReplayStatus replay_type2_unpack_type19_fields(
    const uint8_t *file_data, size_t file_size, const ReplayAe7Movie *movie,
    size_t frame_index, MbFrame *output);

#endif
