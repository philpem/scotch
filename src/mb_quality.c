#include "replay/mb_quality.h"

#include <stddef.h>

/*
 * Copied from PROCReadQualTable in Decomp19's BatchComp source. Each row is
 * {ordinary per-component limit, exceptional luma limit, 4x4 total, 2x2
 * total}. The first row is therefore exact matching, not a special code path.
 */
static const MbQualityThresholds quality_table[MB_QUALITY_LEVEL_COUNT] = {
    { 0U, 0U,   0U,  0U }, { 0U, 1U,   2U,  1U },
    { 0U, 1U,   4U,  2U }, { 1U, 1U,   6U,  2U },
    { 1U, 1U,   8U,  2U }, { 1U, 2U,  10U,  3U },
    { 1U, 2U,  12U,  3U }, { 1U, 2U,  14U,  4U },
    { 1U, 2U,  16U,  4U }, { 1U, 2U,  18U,  5U },
    { 1U, 2U,  20U,  5U }, { 1U, 2U,  24U,  6U },
    { 1U, 2U,  28U,  7U }, { 1U, 3U,  32U,  8U },
    { 1U, 3U,  36U,  9U }, { 1U, 3U,  48U, 12U },
    { 2U, 3U,  48U, 12U }, { 2U, 4U,  48U, 12U },
    { 2U, 4U,  72U, 18U }, { 2U, 5U,  96U, 24U },
    { 2U, 6U,  96U, 24U }, { 3U, 6U, 100U, 28U },
    { 3U, 7U, 132U, 33U }, { 4U, 8U, 144U, 36U },
    { 4U, 9U, 180U, 45U }, { 5U, 10U, 180U, 45U },
    { 5U, 11U, 195U, 48U }, { 6U, 12U, 198U, 50U },
    { 6U, 13U, 216U, 60U }
};

static int signed_chroma(uint8_t value)
{
    return value < 16U ? (int)value : (int)value - 32;
}

static unsigned absolute_difference(int left, int right)
{
    int difference = left - right;
    return (unsigned)(difference < 0 ? -difference : difference);
}

static int floor_average(int sum, unsigned count)
{
    /* Match ARM ASR: negative values round down, unlike C integer division. */
    if (sum >= 0) {
        return sum / (int)count;
    }
    return -((-sum + (int)count - 1) / (int)count);
}

ReplayStatus mb_quality_thresholds(unsigned loss_level,
                                   MbQualityThresholds *thresholds)
{
    if (thresholds == NULL || loss_level >= MB_QUALITY_LEVEL_COUNT) {
        return REPLAY_INVALID_ARGUMENT;
    }
    *thresholds = quality_table[loss_level];
    return REPLAY_OK;
}

int mb_quality_profile_format19(const MbFrame *target, unsigned target_x,
                                unsigned target_y,
                                const MbFrame *reference,
                                unsigned reference_x,
                                unsigned reference_y, unsigned block_size,
                                uint8_t target_u, uint8_t target_v,
                                MbQualityProfile *profile)
{
    unsigned total = 0U;
    int reference_u = 0;
    int reference_v = 0;
    unsigned row;

    if (target == NULL || reference == NULL || profile == NULL ||
        target->pixels == NULL ||
        reference->pixels == NULL ||
        (block_size != 2U && block_size != 4U) ||
        target_x + block_size > target->width ||
        target_y + block_size > target->height ||
        reference_x + block_size > reference->width ||
        reference_y + block_size > reference->height) {
        return 0;
    }
    *profile = (MbQualityProfile){ 0 };

    for (row = 0U; row < block_size; ++row) {
        unsigned column;
        for (column = 0U; column < block_size; ++column) {
            const MbPixel *target_pixel =
                &target->pixels[(size_t)(target_y + row) * target->stride +
                                target_x + column];
            const MbPixel *reference_pixel =
                &reference->pixels[(size_t)(reference_y + row) *
                                       reference->stride +
                                   reference_x + column];
            unsigned difference = absolute_difference(
                target_pixel->y, reference_pixel->y);

            unsigned limit;

            if (difference > profile->max_luma_error) {
                profile->max_luma_error = (uint8_t)difference;
            }
            for (limit = 0U; limit < 7U && difference > limit; ++limit) {
                ++profile->luma_over_limit[limit];
            }
            total += difference;
            reference_u += signed_chroma(reference_pixel->u);
            reference_v += signed_chroma(reference_pixel->v);
        }
    }

    {
        unsigned area = block_size * block_size;
        profile->chroma_u_error = (uint8_t)absolute_difference(
            signed_chroma(target_u), floor_average(reference_u, area));
        profile->chroma_v_error = (uint8_t)absolute_difference(
            signed_chroma(target_v), floor_average(reference_v, area));
        /* One block-average chroma error applies to every reconstructed pixel. */
        total += area * (profile->chroma_u_error + profile->chroma_v_error);
    }
    profile->total_error = (uint16_t)total;
    return 1;
}

int mb_quality_profile_format17(const MbFrame *target, unsigned target_x,
                                unsigned target_y,
                                const MbFrame *reference,
                                unsigned reference_x,
                                unsigned reference_y, unsigned block_size,
                                uint8_t target_u, uint8_t target_v,
                                MbQualityProfile *profile)
{
    unsigned total = 0U;
    unsigned reference_u = 0U;
    unsigned reference_v = 0U;
    unsigned row;

    if (target == NULL || reference == NULL || profile == NULL ||
        target->pixels == NULL ||
        reference->pixels == NULL ||
        (block_size != 2U && block_size != 4U) ||
        target_x + block_size > target->width ||
        target_y + block_size > target->height ||
        reference_x + block_size > reference->width ||
        reference_y + block_size > reference->height) {
        return 0;
    }
    *profile = (MbQualityProfile){ 0 };

    for (row = 0U; row < block_size; ++row) {
        unsigned column;
        for (column = 0U; column < block_size; ++column) {
            const MbPixel *target_pixel =
                &target->pixels[(size_t)(target_y + row) * target->stride +
                                target_x + column];
            const MbPixel *reference_pixel =
                &reference->pixels[(size_t)(reference_y + row) *
                                       reference->stride +
                                   reference_x + column];
            unsigned difference = absolute_difference(
                target_pixel->y, reference_pixel->y);

            unsigned limit;

            if (difference > profile->max_luma_error) {
                profile->max_luma_error = (uint8_t)difference;
            }
            for (limit = 0U; limit < 7U && difference > limit; ++limit) {
                ++profile->luma_over_limit[limit];
            }
            total += difference;
            /* Type 17 chroma is unsigned five-bit, so average without sign. */
            reference_u += reference_pixel->u;
            reference_v += reference_pixel->v;
        }
    }

    {
        unsigned area = block_size * block_size;
        /* Round to match the data block's avg5 chroma, so a copy of an
           identical block reports zero chroma error rather than a half-LSB. */
        unsigned reference_avg_u = (reference_u + area / 2U) / area;
        unsigned reference_avg_v = (reference_v + area / 2U) / area;

        profile->chroma_u_error = (uint8_t)absolute_difference(
            (int)target_u, (int)reference_avg_u);
        profile->chroma_v_error = (uint8_t)absolute_difference(
            (int)target_v, (int)reference_avg_v);
        /* One block-average chroma error applies to every reconstructed pixel. */
        total += area * (profile->chroma_u_error + profile->chroma_v_error);
    }
    profile->total_error = (uint16_t)total;
    return 1;
}

int mb_quality_match_format17(const MbFrame *target, unsigned target_x,
                              unsigned target_y, const MbFrame *reference,
                              unsigned reference_x, unsigned reference_y,
                              unsigned block_size, uint8_t target_u,
                              uint8_t target_v,
                              const MbQualityThresholds *thresholds,
                              unsigned *total_error)
{
    MbQualityProfile profile;

    return mb_quality_profile_format17(
               target, target_x, target_y, reference, reference_x,
               reference_y, block_size, target_u, target_v, &profile) &&
           mb_quality_profile_accept(
               &profile, block_size, thresholds, total_error);
}

unsigned mb_quality_first_accepted_level(const MbQualityProfile *profile,
                                         unsigned block_size)
{
    unsigned low = 0U;
    unsigned high = MB_QUALITY_LEVEL_COUNT - 1U;
    MbQualityThresholds thresholds;
    unsigned error;

    /* The source quality table loosens monotonically from level 0 through 28. */
    (void)mb_quality_thresholds(high, &thresholds);
    if (!mb_quality_profile_accept(
            profile, block_size, &thresholds, &error)) {
        return MB_QUALITY_LEVEL_COUNT;
    }
    while (low < high) {
        unsigned middle = low + (high - low) / 2U;

        (void)mb_quality_thresholds(middle, &thresholds);
        if (mb_quality_profile_accept(
                profile, block_size, &thresholds, &error)) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return low;
}

int mb_quality_profile_accept(const MbQualityProfile *profile,
                              unsigned block_size,
                              const MbQualityThresholds *thresholds,
                              unsigned *total_error)
{
    unsigned exceptional_limit = block_size == 4U ? 4U : 1U;
    unsigned total_limit;

    if (profile == NULL || thresholds == NULL || total_error == NULL ||
        (block_size != 2U && block_size != 4U)) {
        return 0;
    }
    total_limit = block_size == 4U ? thresholds->total_error_4x4
                                   : thresholds->total_error_2x2;
    if (profile->max_luma_error > thresholds->max_exceptional_error ||
        profile->luma_over_limit[thresholds->max_individual_error] >
            exceptional_limit ||
        profile->chroma_u_error > thresholds->max_individual_error ||
        profile->chroma_v_error > thresholds->max_individual_error ||
        profile->total_error > total_limit) {
        return 0;
    }
    *total_error = profile->total_error;
    return 1;
}

int mb_quality_match_format19(const MbFrame *target, unsigned target_x,
                              unsigned target_y, const MbFrame *reference,
                              unsigned reference_x, unsigned reference_y,
                              unsigned block_size, uint8_t target_u,
                              uint8_t target_v,
                              const MbQualityThresholds *thresholds,
                              unsigned *total_error)
{
    MbQualityProfile profile;

    return mb_quality_profile_format19(
               target, target_x, target_y, reference, reference_x,
               reference_y, block_size, target_u, target_v, &profile) &&
           mb_quality_profile_accept(
               &profile, block_size, thresholds, total_error);
}
