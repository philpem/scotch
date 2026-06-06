#include "test_common.h"

#include <stdint.h>

#include "replay/mb_color.h"

static int check_pixel(const uint8_t rgb[3], uint8_t y, uint8_t u, uint8_t v)
{
    MbPixel pixel;
    MbFrame frame = { 1U, 1U, 1U, &pixel };

    CHECK(mb_color_rgb24_to_6y5uv(rgb, 3U, &frame) == REPLAY_OK);
    CHECK(pixel.y == y);
    CHECK(pixel.u == u);
    CHECK(pixel.v == v);
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
    return EXIT_SUCCESS;
}
