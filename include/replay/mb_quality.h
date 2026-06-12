#ifndef MB_QUALITY_H
#define MB_QUALITY_H

#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_status.h"

#define MB_QUALITY_LEVEL_COUNT 29U

/* One row of the QP% table shared by Moving Blocks formats 7, 17, 19 and 20. */
typedef struct {
    /* `maxi`: normal per-component limit. */
    uint8_t max_individual_error;
    /* `maxe`: larger luma limit allowed for 4 pixels in 4x4, 1 in 2x2. */
    uint8_t max_exceptional_error;
    uint16_t total_error_4x4;
    uint16_t total_error_2x2;
} MbQualityThresholds;

/*
 * Threshold-independent measurements for one copy candidate. Keeping this
 * profile separate lets a rate-control retry test the same source/reference
 * pixels against another quality row without measuring every pixel again.
 */
typedef struct {
    uint16_t total_error;
    uint8_t max_luma_error;
    uint8_t luma_over_limit[7];
    uint8_t chroma_u_error;
    uint8_t chroma_v_error;
} MbQualityProfile;

ReplayStatus mb_quality_thresholds(unsigned loss_level,
                                   MbQualityThresholds *thresholds);

int mb_quality_profile_format19(const MbFrame *target, unsigned target_x,
                                unsigned target_y,
                                const MbFrame *reference,
                                unsigned reference_x,
                                unsigned reference_y, unsigned block_size,
                                uint8_t target_u, uint8_t target_v,
                                MbQualityProfile *profile);

int mb_quality_profile_accept(const MbQualityProfile *profile,
                              unsigned block_size,
                              const MbQualityThresholds *thresholds,
                              unsigned *total_error);

/*
 * Score a format-19 copy candidate using the original compressor's metric.
 * `target_u` and `target_v` are the signed-five-bit block averages that a data
 * block would reconstruct. `reference` supplies candidate decoded pixels.
 * A zero return means rejection; a non-zero return stores the total error.
 */
int mb_quality_match_format19(const MbFrame *target, unsigned target_x,
                              unsigned target_y, const MbFrame *reference,
                              unsigned reference_x, unsigned reference_y,
                              unsigned block_size, uint8_t target_u,
                              uint8_t target_v,
                              const MbQualityThresholds *thresholds,
                              unsigned *total_error);

#endif
