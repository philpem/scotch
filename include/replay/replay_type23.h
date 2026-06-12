#ifndef REPLAY_TYPE23_H
#define REPLAY_TYPE23_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_ae7.h"

/* Return the byte-aligned size of one packed 4:2:2, 11-bit/pixel frame. */
ReplayStatus replay_type23_frame_bytes(unsigned width, unsigned height,
                                       size_t *bytes);

ReplayStatus replay_type23_frame_count(const ReplayAe7Movie *movie,
                                       size_t *frame_count);

/*
 * Unpack 6Y6Y5U5V 4:2:2 into one 6Y5UV sample per pixel. Each horizontal pair
 * has independent six-bit luma and one shared signed-five-bit U/V value;
 * chroma is not subsampled vertically.
 */
ReplayStatus replay_type23_unpack_frame(const uint8_t *file_data,
                                        size_t file_size,
                                        const ReplayAe7Movie *movie,
                                        size_t frame_index, MbFrame *output);

/* Pack one frame, averaging each horizontal pair's signed U and V values. */
ReplayStatus replay_type23_pack_frame(const MbFrame *source,
                                      uint8_t *output, size_t output_size);

#endif
