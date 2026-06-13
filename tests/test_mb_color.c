#include "test_common.h"

#include <stdint.h>

#include "replay/mb_color.h"

static int check_pixel(const uint8_t rgb[3], uint8_t y, uint8_t u, uint8_t v)
{
    MbPixel pixel;
    MbFrame frame = { 1U, 1U, 1U, &pixel };

    CHECK(mb_color_rgb24_to_6y5uv(rgb, 3U, &frame, MB_COLOR_DITHER_NONE) == REPLAY_OK);
    CHECK(pixel.y == y);
    CHECK(pixel.u == u);
    CHECK(pixel.v == v);
    return EXIT_SUCCESS;
}

static int check_pixel555(const uint8_t rgb[3], uint8_t y, uint8_t u, uint8_t v)
{
    MbPixel pixel;
    MbFrame frame = { 1U, 1U, 1U, &pixel };

    CHECK(mb_color_rgb24_to_yuv555(rgb, 3U, &frame, MB_COLOR_DITHER_NONE) == REPLAY_OK);
    CHECK(pixel.y == y);
    CHECK(pixel.u == u);
    CHECK(pixel.v == v);
    return EXIT_SUCCESS;
}

/* A flat grey landing exactly between two luma levels: round-to-nearest gives
 * one uniform value, while ordered dithering splits the 4x4 Bayer cell evenly
 * between the two neighbouring levels. */
static int test_ordered_dither(void)
{
    uint8_t grey[16U * 3U];
    MbPixel pixels[16];
    MbFrame frame = { 4U, 4U, 4U, pixels };
    unsigned i;
    unsigned below = 0U;
    unsigned at = 0U;

    for (i = 0U; i < sizeof(grey); ++i) {
        grey[i] = 128U; /* 5-bit luma 128*31/256 = 15.5 */
    }
    CHECK(mb_color_rgb24_to_yuv555(grey, 12U, &frame, MB_COLOR_DITHER_NONE) ==
          REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        CHECK(pixels[i].y == 16U);
    }
    CHECK(mb_color_rgb24_to_yuv555(grey, 12U, &frame, MB_COLOR_DITHER_ORDERED) ==
          REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        if (pixels[i].y == 15U) {
            ++below;
        } else if (pixels[i].y == 16U) {
            ++at;
        }
    }
    CHECK(below == 8U && at == 8U);
    return EXIT_SUCCESS;
}

int main(void)
{
    static const uint8_t black[3] = { 0U, 0U, 0U };
    static const uint8_t white[3] = { 255U, 255U, 255U };
    static const uint8_t red[3] = { 255U, 0U, 0U };
    static const uint8_t green[3] = { 0U, 255U, 0U };
    static const uint8_t blue[3] = { 0U, 0U, 255U };
    MbPixel preview_pixels[2] = {
        { 0U, 0U, 0U }, { 63U, 0U, 0U }
    };
    MbFrame preview_frame = { 2U, 1U, 2U, preview_pixels };
    uint8_t preview_rgb[6];
    MbPixel preview555_pixels[2] = {
        { 0U, 0U, 0U }, { 31U, 0U, 0U }
    };
    MbFrame preview555_frame = { 2U, 1U, 2U, preview555_pixels };
    uint8_t preview555_rgb[6];

    CHECK(check_pixel(black, 0U, 0U, 0U) == EXIT_SUCCESS);
    CHECK(check_pixel(white, 63U, 0U, 0U) == EXIT_SUCCESS);
    CHECK(check_pixel(red, 19U, 26U, 15U) == EXIT_SUCCESS);
    CHECK(check_pixel(green, 37U, 21U, 18U) == EXIT_SUCCESS);
    CHECK(check_pixel(blue, 7U, 15U, 28U) == EXIT_SUCCESS);
    CHECK(mb_color_6y5uv_to_rgb24(&preview_frame, preview_rgb, 6U) ==
          REPLAY_OK);
    CHECK(preview_rgb[0] == 0U && preview_rgb[1] == 0U &&
          preview_rgb[2] == 0U);
    CHECK(preview_rgb[3] == 255U && preview_rgb[4] == 255U &&
          preview_rgb[5] == 255U);

    /* YUV555 shares the 6Y5UV chroma; only luma is rescaled to five bits. */
    CHECK(check_pixel555(black, 0U, 0U, 0U) == EXIT_SUCCESS);
    CHECK(check_pixel555(white, 31U, 0U, 0U) == EXIT_SUCCESS);
    CHECK(check_pixel555(red, 9U, 26U, 15U) == EXIT_SUCCESS);
    CHECK(check_pixel555(green, 18U, 21U, 18U) == EXIT_SUCCESS);
    CHECK(check_pixel555(blue, 4U, 15U, 28U) == EXIT_SUCCESS);
    CHECK(mb_color_yuv555_to_rgb24(&preview555_frame, preview555_rgb, 6U) ==
          REPLAY_OK);
    CHECK(preview555_rgb[0] == 0U && preview555_rgb[1] == 0U &&
          preview555_rgb[2] == 0U);
    CHECK(preview555_rgb[3] == 255U && preview555_rgb[4] == 255U &&
          preview555_rgb[5] == 255U);

    CHECK(test_ordered_dither() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
