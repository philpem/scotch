#include "test_common.h"

#include <stdint.h>

#include "replay/mb_quality.h"

int main(void)
{
    MbQualityThresholds thresholds;
    MbQualityProfile profile;
    MbPixel target_pixels[16] = { { 10U, 0U, 0U } };
    MbPixel reference_pixels[16] = { { 10U, 0U, 0U } };
    MbFrame target = { 4U, 4U, 4U, target_pixels };
    MbFrame reference = { 4U, 4U, 4U, reference_pixels };
    unsigned error;
    unsigned profiled_error;
    unsigned level;
    size_t i;

    for (i = 0U; i < 16U; ++i) {
        target_pixels[i] = (MbPixel){ 10U, 0U, 0U };
        reference_pixels[i] = target_pixels[i];
    }

    CHECK(mb_quality_thresholds(0U, &thresholds) == REPLAY_OK);
    CHECK(thresholds.max_individual_error == 0U);
    CHECK(thresholds.total_error_4x4 == 0U);
    CHECK(mb_quality_match_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &thresholds, &error));
    CHECK(error == 0U);
    reference_pixels[0].y = 11U;
    CHECK(!mb_quality_match_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &thresholds, &error));

    /* A cached profile must reproduce the direct matcher at every row. */
    CHECK(mb_quality_profile_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &profile));
    for (level = 0U; level < MB_QUALITY_LEVEL_COUNT; ++level) {
        int direct;
        int cached;

        CHECK(mb_quality_thresholds(level, &thresholds) == REPLAY_OK);
        direct = mb_quality_match_format19(
            &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
            &thresholds, &error);
        cached = mb_quality_profile_accept(
            &profile, 4U, &thresholds, &profiled_error);
        CHECK(direct == cached);
        CHECK(!direct || error == profiled_error);
    }

    CHECK(mb_quality_thresholds(7U, &thresholds) == REPLAY_OK);
    CHECK(thresholds.max_individual_error == 1U);
    CHECK(thresholds.max_exceptional_error == 2U);
    CHECK(thresholds.total_error_4x4 == 14U);
    CHECK(thresholds.total_error_2x2 == 4U);
    for (i = 0U; i < 4U; ++i) {
        reference_pixels[i].y = 12U;
    }
    CHECK(mb_quality_match_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &thresholds, &error));
    CHECK(error == 8U);
    reference_pixels[4].y = 12U;
    CHECK(!mb_quality_match_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &thresholds, &error));

    for (i = 0U; i < 16U; ++i) {
        reference_pixels[i] = target_pixels[i];
        reference_pixels[i].u = 1U;
    }
    CHECK(!mb_quality_match_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 0U, 0U,
              &thresholds, &error));
    CHECK(mb_quality_thresholds(28U, &thresholds) == REPLAY_OK);
    CHECK(thresholds.max_exceptional_error == 13U);
    CHECK(thresholds.total_error_4x4 == 216U);
    CHECK(mb_quality_thresholds(29U, &thresholds) == REPLAY_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}
