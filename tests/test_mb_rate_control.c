#include "test_common.h"

#include "replay/mb_rate_control.h"

int main(void)
{
    MbRateControl control;

    CHECK(mb_rate_control_init(&control, 1000U, 7U) == REPLAY_OK);
    CHECK(control.target_min_bytes == 900U);
    CHECK(control.target_max_bytes == 1025U);
    CHECK(mb_rate_control_observe(&control, 1100U) == MB_RATE_RETRY);
    CHECK(control.loss_level == 8U && control.direction == 1);
    CHECK(mb_rate_control_observe(&control, 1050U) == MB_RATE_RETRY);
    CHECK(control.loss_level == 9U && control.retry_count == 2U);
    /* Crossing below the lower bound is accepted rather than reversing. */
    CHECK(mb_rate_control_observe(&control, 850U) == MB_RATE_ACCEPT);

    CHECK(mb_rate_control_init(&control, 101U, 2U) == REPLAY_OK);
    CHECK(control.target_min_bytes == 90U);
    CHECK(control.target_max_bytes == 103U);
    CHECK(mb_rate_control_observe(&control, 80U) == MB_RATE_RETRY);
    CHECK(control.loss_level == 1U && control.direction == -1);
    CHECK(mb_rate_control_observe(&control, 85U) == MB_RATE_RETRY);
    CHECK(control.loss_level == 0U);
    CHECK(mb_rate_control_observe(&control, 85U) == MB_RATE_ACCEPT);

    CHECK(mb_rate_control_init(NULL, 100U, 0U) == REPLAY_INVALID_ARGUMENT);
    CHECK(mb_rate_control_init(&control, 0U, 0U) == REPLAY_INVALID_ARGUMENT);
    CHECK(mb_rate_control_init(&control, 100U, 29U) ==
          REPLAY_INVALID_ARGUMENT);

    /* Custom factors use explicit floating-point truncation toward zero. */
    CHECK(mb_rate_control_init_window(
              &control, 101U, 4U, 0.875, 1.125) == REPLAY_OK);
    CHECK(control.target_min_bytes == 88U);
    CHECK(control.target_max_bytes == 113U);
    CHECK(mb_rate_control_init_window(
              &control, 100U, 4U, 1.1, 1.0) == REPLAY_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}
