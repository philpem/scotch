#include "test_common.h"

#include <stdint.h>

#include "replay/mb_quality.h"

static int test_format19(void)
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

static int test_first_accepted_level(void)
{
    MbQualityProfile profile;
    MbPixel target_pixels[16];
    MbPixel reference_pixels[16];
    MbFrame target = { 4U, 4U, 4U, target_pixels };
    MbFrame reference = { 4U, 4U, 4U, reference_pixels };
    size_t i;

    /* An exact profile is accepted by the strictest row, level 0. */
    for (i = 0U; i < 16U; ++i) {
        target_pixels[i] = (MbPixel){ 12U, 4U, 4U };
        reference_pixels[i] = target_pixels[i];
    }
    CHECK(mb_quality_profile_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 4U, 4U, &profile));
    CHECK(mb_quality_first_accepted_level(&profile, 4U) == 0U);

    /* A small error needs a looser row; the level is monotonic and bounded. */
    reference_pixels[0].y = 14U;
    CHECK(mb_quality_profile_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 4U, 4U, &profile));
    {
        unsigned level = mb_quality_first_accepted_level(&profile, 4U);
        MbQualityThresholds thresholds;
        unsigned error;

        CHECK(level > 0U && level < MB_QUALITY_LEVEL_COUNT);
        CHECK(mb_quality_thresholds(level, &thresholds) == REPLAY_OK);
        CHECK(mb_quality_profile_accept(&profile, 4U, &thresholds, &error));
        CHECK(mb_quality_thresholds(level - 1U, &thresholds) == REPLAY_OK);
        CHECK(!mb_quality_profile_accept(&profile, 4U, &thresholds, &error));
    }

    /* An error no row tolerates returns the past-the-end sentinel. */
    for (i = 0U; i < 16U; ++i) {
        reference_pixels[i].y = (uint8_t)(i & 1U ? 31U : 0U);
    }
    CHECK(mb_quality_profile_format19(
              &target, 0U, 0U, &reference, 0U, 0U, 4U, 4U, 4U, &profile));
    CHECK(mb_quality_first_accepted_level(&profile, 4U) ==
          MB_QUALITY_LEVEL_COUNT);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_format19() == EXIT_SUCCESS);
    CHECK(test_first_accepted_level() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
