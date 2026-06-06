#ifndef MB_METRICS_H
#define MB_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "replay/mb_frame.h"
#include "replay/replay_status.h"

typedef struct {
    size_t pixel_count;
    uint64_t squared_error_y;
    uint64_t squared_error_u;
    uint64_t squared_error_v;
    unsigned max_error_y;
    unsigned max_error_u;
    unsigned max_error_v;
} MbFrameMetrics;

/* Compare decoder-visible 6Y5UV, interpreting five-bit chroma as signed. */
ReplayStatus mb_metrics_compare_6y5uv(const MbFrame *reference,
                                      const MbFrame *actual,
                                      MbFrameMetrics *metrics);

ReplayStatus mb_metrics_compare_6y5uv_region(
    const MbFrame *reference, const MbFrame *actual,
    unsigned x, unsigned y, unsigned width, unsigned height,
    MbFrameMetrics *metrics);

double mb_metrics_mse(uint64_t squared_error, size_t sample_count);
double mb_metrics_psnr(uint64_t squared_error, size_t sample_count,
                       unsigned peak_value);

#endif
