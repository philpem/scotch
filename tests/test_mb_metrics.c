#include "test_common.h"

#include <math.h>

#include "replay/mb_metrics.h"

int main(void)
{
    MbPixel reference_pixels[2] = {
        { 10U, 15U, 16U }, { 20U, 31U, 0U }
    };
    MbPixel actual_pixels[2] = {
        { 13U, 14U, 17U }, { 18U, 0U, 31U }
    };
    MbFrame reference = { 2U, 1U, 2U, reference_pixels };
    MbFrame actual = { 2U, 1U, 2U, actual_pixels };
    MbFrameMetrics metrics;

    CHECK(mb_metrics_compare_6y5uv(&reference, &actual, &metrics) ==
          REPLAY_OK);
    CHECK(metrics.pixel_count == 2U);
    CHECK(metrics.squared_error_y == 13U);
    CHECK(metrics.squared_error_u == 2U);
    CHECK(metrics.squared_error_v == 2U);
    CHECK(metrics.max_error_y == 3U);
    CHECK(metrics.max_error_u == 1U);
    CHECK(metrics.max_error_v == 1U);
    CHECK(fabs(mb_metrics_mse(metrics.squared_error_y, 2U) - 6.5) < 0.001);
    CHECK(isinf(mb_metrics_psnr(0U, 2U, 63U)));
    CHECK(mb_metrics_compare_6y5uv_region(
              &reference, &actual, 1U, 0U, 1U, 1U, &metrics) == REPLAY_OK);
    CHECK(metrics.pixel_count == 1U && metrics.squared_error_y == 4U);
    CHECK(metrics.squared_error_u == 1U && metrics.squared_error_v == 1U);
    return EXIT_SUCCESS;
}
