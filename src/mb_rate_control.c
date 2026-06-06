#include "replay/mb_rate_control.h"

#include "replay/mb_quality.h"

ReplayStatus mb_rate_control_init(MbRateControl *control, size_t target_bytes,
                                  unsigned initial_loss_level)
{
    size_t tenths;
    size_t remainder;

    if (control == NULL || target_bytes == 0U ||
        initial_loss_level >= MB_QUALITY_LEVEL_COUNT ||
        target_bytes > SIZE_MAX - target_bytes / 40U) {
        return REPLAY_INVALID_ARGUMENT;
    }

    /* BASIC assigned 0.90*target and 1.025*target to integer limits. */
    tenths = target_bytes / 10U;
    remainder = target_bytes % 10U;
    control->target_min_bytes = tenths * 9U + remainder * 9U / 10U;
    control->target_max_bytes = target_bytes + target_bytes / 40U;
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
