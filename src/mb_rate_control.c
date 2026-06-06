#include "replay/mb_rate_control.h"

#include <math.h>

#include "replay/mb_quality.h"

ReplayStatus mb_rate_control_init(MbRateControl *control, size_t target_bytes,
                                  unsigned initial_loss_level)
{
    return mb_rate_control_init_window(
        control, target_bytes, initial_loss_level,
        MB_RATE_DEFAULT_MIN_FACTOR, MB_RATE_DEFAULT_MAX_FACTOR);
}

ReplayStatus mb_rate_control_init_window(MbRateControl *control,
                                         size_t target_bytes,
                                         unsigned initial_loss_level,
                                         double minimum_factor,
                                         double maximum_factor)
{
    double minimum_bytes = (double)target_bytes * minimum_factor;
    double maximum_bytes = (double)target_bytes * maximum_factor;

    if (control == NULL || target_bytes == 0U ||
        initial_loss_level >= MB_QUALITY_LEVEL_COUNT ||
        !isfinite(minimum_factor) || !isfinite(maximum_factor) ||
        minimum_factor < 0.0 || maximum_factor < minimum_factor ||
        !isfinite(minimum_bytes) || !isfinite(maximum_bytes) ||
        maximum_bytes >= (double)SIZE_MAX) {
        return REPLAY_INVALID_ARGUMENT;
    }

    /* Express the BASIC real formulas directly, then make truncation explicit. */
    control->target_min_bytes = (size_t)trunc(minimum_bytes);
    control->target_max_bytes = (size_t)trunc(maximum_bytes);
    control->loss_level = initial_loss_level;
    control->retry_count = 0U;
    control->direction = 0;
    return REPLAY_OK;
}

MbRateDecision mb_rate_control_observe(MbRateControl *control,
                                       size_t encoded_bytes)
{
    if (control == NULL) {
        return MB_RATE_ACCEPT;
    }

    /*
     * Once a direction is chosen, do not reverse it after crossing the target
     * window. This mirrors PROCmatch and prevents adjacent quality rows from
     * making the retry loop oscillate forever.
     */
    if (control->direction <= 0 &&
        encoded_bytes < control->target_min_bytes &&
        control->loss_level > 0U) {
        --control->loss_level;
        control->direction = -1;
        ++control->retry_count;
        return MB_RATE_RETRY;
    }
    if (control->direction >= 0 &&
        encoded_bytes > control->target_max_bytes &&
        control->loss_level + 1U < MB_QUALITY_LEVEL_COUNT) {
        ++control->loss_level;
        control->direction = 1;
        ++control->retry_count;
        return MB_RATE_RETRY;
    }
    return MB_RATE_ACCEPT;
}
