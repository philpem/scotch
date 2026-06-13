#ifndef MB_COLOR_H
#define MB_COLOR_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_status.h"

/*
 * Luma quantisation mode for the RGB->YUV conversions. MB_COLOR_DITHER_NONE is
 * Acorn's straight round-to-nearest. The ORDERED modes add a fixed Bayer
 * threshold so a gradient breaks into a stable spatial pattern instead of hard
 * bands -- useful for the family's coarse 5/6-bit luma. The 8x8 matrix is finer
 * than the 4x4, so its pattern is less visible at the cost of slightly more
 * varied (less compressible) output. Only luma is dithered: chroma is
 * block-averaged by the codecs, so a per-pixel chroma dither would be averaged
 * away. Dithering trades banding for ~1 LSB of noise.
 */
typedef enum {
    MB_COLOR_DITHER_NONE,
    MB_COLOR_DITHER_ORDERED_4X4,
    MB_COLOR_DITHER_ORDERED_8X8
} MbColorDither;

/* Convert packed R,G,B bytes using CompLib's 6Y5UV path. */
ReplayStatus mb_color_rgb24_to_6y5uv(const uint8_t *rgb, size_t rgb_stride,
                                    MbFrame *output, MbColorDither dither);

/* Convert 6Y5UV to display RGB; this is a preview, not a reversible mapping. */
ReplayStatus mb_color_6y5uv_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                    size_t rgb_stride);

/*
 * Type 17 YUV555: 5-bit luma with the same signed five-bit U/V chroma as 6Y5UV.
 * These mirror the 6Y5UV pair with luma scaled to five bits.
 */
ReplayStatus mb_color_rgb24_to_yuv555(const uint8_t *rgb, size_t rgb_stride,
                                      MbFrame *output, MbColorDither dither);

ReplayStatus mb_color_yuv555_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                      size_t rgb_stride);

#endif
