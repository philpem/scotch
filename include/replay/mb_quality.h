#ifndef MB_QUALITY_H
#define MB_QUALITY_H

#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_status.h"

#define MB_QUALITY_LEVEL_COUNT 29U

/* One row of the QP% table shared by Moving Blocks formats 7, 17, 19 and 20. */
typedef struct {
    uint8_t max_individual_error;
    uint8_t max_exceptional_error;
    uint16_t total_error_4x4;
    uint16_t total_error_2x2;
} MbQualityThresholds;

ReplayStatus mb_quality_thresholds(unsigned loss_level,
                                   MbQualityThresholds *thresholds);

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
