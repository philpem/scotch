#ifndef MB_COLOR_H
#define MB_COLOR_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_status.h"

/* Convert packed R,G,B bytes using CompLib's non-dithered 6Y5UV path. */
ReplayStatus mb_color_rgb24_to_6y5uv(const uint8_t *rgb, size_t rgb_stride,
                                    MbFrame *output);

/* Convert 6Y5UV to display RGB; this is a preview, not a reversible mapping. */
ReplayStatus mb_color_6y5uv_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                    size_t rgb_stride);

/*
 * Type 17 YUV555: 5-bit luma with the same signed five-bit U/V chroma as 6Y5UV.
 * These mirror the 6Y5UV pair with luma scaled to five bits.
 */
ReplayStatus mb_color_rgb24_to_yuv555(const uint8_t *rgb, size_t rgb_stride,
                                      MbFrame *output);

ReplayStatus mb_color_yuv555_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                      size_t rgb_stride);

#endif
