#include "replay/mb_metrics.h"

#include <math.h>

static int signed_chroma(uint8_t value)
{
    return value < 16U ? (int)value : (int)value - 32;
}

static unsigned absolute_difference(int left, int right)
{
    int difference = left - right;
    return (unsigned)(difference < 0 ? -difference : difference);
}

ReplayStatus mb_metrics_compare_6y5uv(const MbFrame *reference,
                                      const MbFrame *actual,
                                      MbFrameMetrics *metrics)
{
    if (reference == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    return mb_metrics_compare_6y5uv_region(
        reference, actual, 0U, 0U, reference->width, reference->height,
        metrics);
}

ReplayStatus mb_metrics_compare_6y5uv_region(
    const MbFrame *reference, const MbFrame *actual,
    unsigned region_x, unsigned region_y, unsigned region_width,
    unsigned region_height, MbFrameMetrics *metrics)
{
    unsigned y;

    if (reference == NULL || actual == NULL || metrics == NULL ||
        reference->pixels == NULL || actual->pixels == NULL ||
        reference->width == 0U || reference->height == 0U ||
        (size_t)reference->width > SIZE_MAX / (size_t)reference->height ||
        reference->width != actual->width ||
        reference->height != actual->height ||
        reference->stride < reference->width ||
        actual->stride < actual->width || region_width == 0U ||
        region_height == 0U || region_x > reference->width ||
        region_y > reference->height ||
        region_width > reference->width - region_x ||
        region_height > reference->height - region_y) {
        return REPLAY_INVALID_ARGUMENT;
    }
    *metrics = (MbFrameMetrics){ 0 };
    metrics->pixel_count = (size_t)region_width * (size_t)region_height;

    for (y = region_y; y < region_y + region_height; ++y) {
        unsigned x;
        for (x = region_x; x < region_x + region_width; ++x) {
            const MbPixel *r =
                &reference->pixels[(size_t)y * reference->stride + x];
            const MbPixel *a =
                &actual->pixels[(size_t)y * actual->stride + x];
            unsigned dy;
            unsigned du;
            unsigned dv;

            if (r->y > 63U || a->y > 63U || r->u > 31U || a->u > 31U ||
                r->v > 31U || a->v > 31U) {
                return REPLAY_INVALID_ARGUMENT;
            }
            dy = absolute_difference((int)r->y, (int)a->y);
            du = absolute_difference(signed_chroma(r->u),
                                     signed_chroma(a->u));
            dv = absolute_difference(signed_chroma(r->v),
                                     signed_chroma(a->v));
            metrics->squared_error_y += (uint64_t)dy * dy;
            metrics->squared_error_u += (uint64_t)du * du;
            metrics->squared_error_v += (uint64_t)dv * dv;
            if (dy > metrics->max_error_y) {
                metrics->max_error_y = dy;
            }
            if (du > metrics->max_error_u) {
                metrics->max_error_u = du;
            }
            if (dv > metrics->max_error_v) {
                metrics->max_error_v = dv;
            }
        }
    }
    return REPLAY_OK;
}

double mb_metrics_mse(uint64_t squared_error, size_t sample_count)
{
    return sample_count == 0U
               ? NAN
               : (double)squared_error / (double)sample_count;
}

double mb_metrics_psnr(uint64_t squared_error, size_t sample_count,
                       unsigned peak_value)
{
    double mse;

    if (sample_count == 0U || peak_value == 0U) {
        return NAN;
    }
    if (squared_error == 0U) {
        return INFINITY;
    }
    mse = mb_metrics_mse(squared_error, sample_count);
    return 10.0 * log10(((double)peak_value * (double)peak_value) / mse);
}
