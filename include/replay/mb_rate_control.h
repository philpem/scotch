#ifndef MB_RATE_CONTROL_H
#define MB_RATE_CONTROL_H

#include <stddef.h>

#include "replay/replay_status.h"

typedef enum {
    MB_RATE_ACCEPT,
    MB_RATE_RETRY
} MbRateDecision;

/*
 * Whole-frame rate control used by the original Moving Blocks compressors.
 * The codec itself remains deterministic and single-pass; this state machine
 * only decides whether the caller should encode the source frame again with a
 * stricter or looser row of the shared 29-level quality table.
 */
typedef struct {
    size_t target_min_bytes;
    size_t target_max_bytes;
    unsigned loss_level;
    unsigned retry_count;
    int direction;
} MbRateControl;

ReplayStatus mb_rate_control_init(MbRateControl *control, size_t target_bytes,
                                  unsigned initial_loss_level);

MbRateDecision mb_rate_control_observe(MbRateControl *control,
                                       size_t encoded_bytes);

#endif
