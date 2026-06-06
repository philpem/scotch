#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

static int test_data4x4_zero_residual_golden(void)
{
    /* Header: opcode 1, U=3, V=5. Then sixteen residual-0 codes (%10). */
    static const uint8_t payload[] = {
        UINT8_C(0x8d), UINT8_C(0xa2), UINT8_C(0xaa),
        UINT8_C(0xaa), UINT8_C(0xaa), UINT8_C(0x0a)
    };
    ReplayBitReader reader;
    MbPredictor predictor = { 0 };
    MbPixel pixels[16];
    MbVerifyError error;
    size_t i;

    memset(pixels, 0xff, sizeof(pixels));
    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_supermovingblocks_decode_data4x4(
              &reader, &predictor, pixels, 4U, &error) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 44U);
    CHECK(predictor.luma == 0U);
    for (i = 0; i < 16U; ++i) {
        CHECK(pixels[i].y == 0U);
        CHECK(pixels[i].u == 3U);
        CHECK(pixels[i].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_data2x2_zero_residual_golden(void)
{
    /* Header: opcode 2, U=3, V=5. Then four residual-0 codes (%10). */
    static const uint8_t payload[] = {
        UINT8_C(0x8e), UINT8_C(0xa2), UINT8_C(0x0a)
    };
    ReplayBitReader reader;
    MbPredictor predictor = { 7 };
    MbPixel pixels[4];
    MbVerifyError error;
    size_t i;

    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_supermovingblocks_decode_data2x2(
              &reader, &predictor, pixels, 2U, &error) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 20U);
    CHECK(predictor.luma == 7U);
    for (i = 0; i < 4U; ++i) {
        CHECK(pixels[i].y == 7U);
        CHECK(pixels[i].u == 3U);
        CHECK(pixels[i].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_predictor_wrap_and_truncation(void)
{
    static const uint8_t wrap_payload[] = {
        /* opcode 2, U=0, V=0, residuals 1, 0, 0, 0 */
        UINT8_C(0x02), UINT8_C(0x70), UINT8_C(0x15)
    };
    static const uint8_t truncated[] = { UINT8_C(0x8d) };
    ReplayBitReader reader;
    MbPredictor predictor = { 63 };
    MbPixel pixels[4];
    MbVerifyError error;

    replay_bitreader_init(&reader, wrap_payload, sizeof(wrap_payload));
    CHECK(codec_supermovingblocks_decode_data2x2(
              &reader, &predictor, pixels, 2U, &error) == REPLAY_OK);
    CHECK(pixels[0].y == 0U);
    CHECK(pixels[0].u == 0U);
    CHECK(pixels[0].v == 0U);

    predictor.luma = 0U;
    replay_bitreader_init(&reader, truncated, sizeof(truncated));
    CHECK(codec_supermovingblocks_decode_data4x4(
              &reader, &predictor, pixels, 2U, &error) ==
          REPLAY_INVALID_ARGUMENT);

    replay_bitreader_init(&reader, truncated, sizeof(truncated));
    CHECK(codec_supermovingblocks_decode_data4x4(
              &reader, &predictor, pixels, 4U, &error) ==
          REPLAY_TRUNCATED_INPUT);
    CHECK(error.detail != NULL);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data4x4_zero_residual_golden() == EXIT_SUCCESS);
    CHECK(test_data2x2_zero_residual_golden() == EXIT_SUCCESS);
    CHECK(test_predictor_wrap_and_truncation() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
