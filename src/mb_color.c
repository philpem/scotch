#include "replay/mb_color.h"

#include <stdint.h>

/*
 * Rounded 16.16 forms of the constants assembled by CompLib. They implement
 * its non-dithered RGB conversion, not a generic full-range YUV transform.
 * Emulator fixtures still need to confirm the original assembler's exact
 * real-to-integer rounding at coefficient boundaries.
 */
#define MB_Y_R 19595
#define MB_Y_G 38470
#define MB_Y_B 7471
#define MB_U_R 22117
#define MB_U_G 43419
#define MB_V_G 54878
#define MB_V_B 10658

static int floor_div_pow2(int value, unsigned shift)
{
    int divisor = 1 << shift;

    /* C division truncates toward zero; ARM ASR rounds negatives downward. */
    if (value >= 0) {
        return value / divisor;
    }
    return -((-value + divisor - 1) / divisor);
}

static uint8_t quantise_chroma(int value)
{
    int scaled = value * 31;

    /* Reproduce CompLib's sign-dependent half-step before its arithmetic ASR. */
    scaled += scaled >= 0 ? 128 : -128;
    return (uint8_t)(floor_div_pow2(scaled, 8U) & 31);
}

static int signed_chroma(uint8_t value)
{
    return value < 16U ? (int)value : (int)value - 32;
}

static int divide_nearest(int value, int divisor)
{
    return value >= 0 ? (value + divisor / 2) / divisor
                      : -((-value + divisor / 2) / divisor);
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > 255) {
        return 255U;
    }
    return (uint8_t)value;
}

/*
 * Ordered 4x4 Bayer matrix, values 0..15. The luma quantiser scales these to
 * the rounding domain: the centre value 8 reproduces straight round-to-nearest,
 * so the matrix spreads the threshold +/- half a quantiser step around it.
 */
static const uint8_t bayer4x4[4][4] = {
    {  0U,  8U,  2U, 10U },
    { 12U,  4U, 14U,  6U },
    {  3U, 11U,  1U,  9U },
    { 15U,  7U, 13U,  5U }
};

/*
 * The Moving Blocks family shares one RGB<->YUV model; only the luma bit depth
 * differs (6Y5UV has 6-bit Y, type 17 YUV555 has 5-bit Y, and U/V are the same
 * signed five-bit chroma throughout). These two workhorses take `luma_max`
 * (63 or 31) so each named format is a one-line wrapper. `dither` controls only
 * the luma rounding (see MbColorDither).
 */
static ReplayStatus rgb24_to_yuv(const uint8_t *rgb, size_t rgb_stride,
                                 MbFrame *output, int luma_max,
                                 MbColorDither dither)
{
    unsigned y;

    if (rgb == NULL || output == NULL || output->pixels == NULL ||
        output->width == 0U || output->height == 0U ||
        output->stride < output->width ||
        rgb_stride < (size_t)output->width * 3U) {
        return REPLAY_INVALID_ARGUMENT;
    }

    for (y = 0; y < output->height; ++y) {
        unsigned x;
        const uint8_t *source = rgb + (size_t)y * rgb_stride;

        for (x = 0; x < output->width; ++x) {
            int red = source[x * 3U];
            int green = source[x * 3U + 1U];
            int blue = source[x * 3U + 2U];
            int fixed_y = red * MB_Y_R + green * MB_Y_G + blue * MB_Y_B;
            /* CompLib derives U from B-Y and V from R-Y using scaled partial Y. */
            int u_base = (red * MB_U_R + green * MB_U_G) >> 16U;
            int v_base = (green * MB_V_G + blue * MB_V_B) >> 16U;
            int u = floor_div_pow2(blue - u_base, 1U);
            int v = floor_div_pow2(red - v_base, 1U);
            MbPixel *pixel = &output->pixels[(size_t)y * output->stride + x];
            /* The quantiser truncates fixed_y*luma_max >> 24, so the rounding
               threshold lives in bits 0..23. 1<<23 is the round-to-nearest
               half step; a Bayer value b replaces it with b<<20, centred on
               1<<23 at b==8. */
            int threshold = dither == MB_COLOR_DITHER_ORDERED
                                ? (int)bayer4x4[y & 3U][x & 3U] << 20
                                : (1 << 23);

            pixel->y = (uint8_t)(((int64_t)fixed_y * luma_max + threshold)
                                 >> 24U);
            pixel->u = quantise_chroma(u);
            pixel->v = quantise_chroma(v);
        }
    }
    return REPLAY_OK;
}

/* Preview inverse of rgb24_to_yuv; the quantisation is irreversible, so this is
 * for display only, not a round trip. `luma_max` matches the forward path. */
static ReplayStatus yuv_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                 size_t rgb_stride, int luma_max)
{
    unsigned y;

    if (input == NULL || input->pixels == NULL || rgb == NULL ||
        input->width == 0U || input->height == 0U ||
        input->stride < input->width ||
        rgb_stride < (size_t)input->width * 3U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (y = 0; y < input->height; ++y) {
        unsigned x;
        uint8_t *destination = rgb + (size_t)y * rgb_stride;

        for (x = 0; x < input->width; ++x) {
            const MbPixel *pixel =
                &input->pixels[(size_t)y * input->stride + x];
            int luma;
            int u;
            int v;

            if ((int)pixel->y > luma_max || pixel->u > 31U || pixel->v > 31U) {
                return REPLAY_INVALID_ARGUMENT;
            }
            luma = divide_nearest((int)pixel->y * 255, luma_max);
            u = divide_nearest(signed_chroma(pixel->u) * 256, 31);
            v = divide_nearest(signed_chroma(pixel->v) * 256, 31);
            destination[x * 3U] =
                clamp_u8(luma + divide_nearest(1402 * v, 1000));
            destination[x * 3U + 1U] =
                clamp_u8(luma - divide_nearest(344 * u + 714 * v, 1000));
            destination[x * 3U + 2U] =
                clamp_u8(luma + divide_nearest(1772 * u, 1000));
        }
    }
    return REPLAY_OK;
}

ReplayStatus mb_color_rgb24_to_6y5uv(const uint8_t *rgb, size_t rgb_stride,
                                    MbFrame *output, MbColorDither dither)
{
    return rgb24_to_yuv(rgb, rgb_stride, output, 63, dither);
}

ReplayStatus mb_color_6y5uv_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                    size_t rgb_stride)
{
    return yuv_to_rgb24(input, rgb, rgb_stride, 63);
}

ReplayStatus mb_color_rgb24_to_yuv555(const uint8_t *rgb, size_t rgb_stride,
                                      MbFrame *output, MbColorDither dither)
{
    return rgb24_to_yuv(rgb, rgb_stride, output, 31, dither);
}

ReplayStatus mb_color_yuv555_to_rgb24(const MbFrame *input, uint8_t *rgb,
                                      size_t rgb_stride)
{
    return yuv_to_rgb24(input, rgb, rgb_stride, 31);
}

#undef MB_Y_R
#undef MB_Y_G
#undef MB_Y_B
#undef MB_U_R
#undef MB_U_G
#undef MB_V_G
#undef MB_V_B
