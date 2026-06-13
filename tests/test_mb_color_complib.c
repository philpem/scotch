#include "test_common.h"

#include <stdint.h>

#include "replay/mb_color.h"

/*
 * Independent check that mb_color's RGB->YUV conversion faithfully implements
 * CompLib's arithmetic (Tools/CompLib in ARMovie_2003, the RGB->YUV routine at
 * BASIC lines 12950-13920). The reference below is written straight from that
 * ARM assembly: a 16.16 luma MLA, U=(B-(R*UxR+G*UxG)>>16)>>1 and the symmetric
 * V, then the format's luma quantiser ((Y*65536)*max + (1<<23) >> 24) and the
 * five-bit signed chroma quantiser (c*31, sign-dependent +/-128 half step,
 * >>8, &31). CompLib applies Floyd-Steinberg error diffusion when dithering;
 * mb_color uses ordered/no dither by design, so only the undithered path is
 * compared.
 *
 * The seven coefficients are CompLib's `DCD 65536*<coeff>` words. All seven are
 * the round-to-nearest of those expressions, confirmed against BBC BASIC's exact
 * float values (e.g. 65536*0.587 = 38469.632 -> 38470, 65536*0.299/0.886 =
 * 22116.551 -> 22117, 65536*0.114/0.701 = 10657.780 -> 10658). This test pins
 * the algorithm; the constants are pinned by that derivation.
 */
#define C_Y_R 19595
#define C_Y_G 38470
#define C_Y_B 7471
#define C_U_R 22117
#define C_U_G 43419
#define C_V_G 54878
#define C_V_B 10658

static uint8_t chroma5(int c)
{
    int scaled = c * 31;

    scaled += scaled >= 0 ? 128 : -128;
    return (uint8_t)((scaled >> 8) & 31); /* ASR and LSR agree after &31 */
}

static void complib_convert(int r, int g, int b, int luma_max,
                            uint8_t *yy, uint8_t *uu, uint8_t *vv)
{
    long y16 = (long)r * C_Y_R + (long)g * C_Y_G + (long)b * C_Y_B;
    long u_full = (long)r * C_U_R + (long)g * C_U_G;
    long v_full = (long)g * C_V_G + (long)b * C_V_B;
    int u = (b - (int)(u_full >> 16)) >> 1;
    int v = (r - (int)(v_full >> 16)) >> 1;

    *yy = (uint8_t)(((long long)y16 * luma_max + (1 << 23)) >> 24);
    *uu = chroma5(u);
    *vv = chroma5(v);
}

/* Convert a sweep of RGB triples and compare mb_color with the reference. */
static int sweep(int luma_max, int is_6y5uv)
{
    enum { STEPS = 18 };
    static uint8_t rgb[STEPS * STEPS * STEPS * 3];
    static MbPixel pixels[STEPS * STEPS * STEPS];
    MbFrame frame = { STEPS * STEPS * STEPS, 1U, STEPS * STEPS * STEPS, pixels };
    size_t i = 0U;
    unsigned ri;

    for (ri = 0U; ri < STEPS; ++ri) {
        unsigned gi;
        for (gi = 0U; gi < STEPS; ++gi) {
            unsigned bi;
            for (bi = 0U; bi < STEPS; ++bi) {
                rgb[i * 3U] = (uint8_t)(ri * 255U / (STEPS - 1U));
                rgb[i * 3U + 1U] = (uint8_t)(gi * 255U / (STEPS - 1U));
                rgb[i * 3U + 2U] = (uint8_t)(bi * 255U / (STEPS - 1U));
                ++i;
            }
        }
    }
    if (is_6y5uv) {
        CHECK(mb_color_rgb24_to_6y5uv(rgb, frame.width * 3U, &frame,
                                      MB_COLOR_DITHER_NONE) == REPLAY_OK);
    } else {
        CHECK(mb_color_rgb24_to_yuv555(rgb, frame.width * 3U, &frame,
                                       MB_COLOR_DITHER_NONE) == REPLAY_OK);
    }
    for (i = 0U; i < frame.width; ++i) {
        uint8_t y;
        uint8_t u;
        uint8_t v;

        complib_convert(rgb[i * 3U], rgb[i * 3U + 1U], rgb[i * 3U + 2U],
                        luma_max, &y, &u, &v);
        CHECK(pixels[i].y == y);
        CHECK(pixels[i].u == u);
        CHECK(pixels[i].v == v);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(sweep(63, 1) == EXIT_SUCCESS); /* 6Y5UV: 6-bit luma */
    CHECK(sweep(31, 0) == EXIT_SUCCESS); /* YUV555: 5-bit luma */
    return EXIT_SUCCESS;
}
