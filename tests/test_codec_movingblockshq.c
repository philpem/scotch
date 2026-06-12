#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_movingblockshq.h"
#include "replay/mb_motion.h"

static void fill_previous(MbPixel *pixels, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        pixels[index].y = (uint8_t)(index & 31U);
        pixels[index].u = (uint8_t)((index + 3U) & 31U);
        pixels[index].v = (uint8_t)((index + 7U) & 31U);
    }
}

static int same_pixel(const MbPixel *left, const MbPixel *right)
{
    return left->y == right->y && left->u == right->u &&
           left->v == right->v;
}

static int test_data4x4_zero_residual(void)
{
    static const uint8_t payload[] = {
        0x8dU, 0xa2U, 0xaaU, 0xaaU, 0xaaU, 0x0aU
    };
    ReplayBitReader reader;
    MbPredictor predictor = { 7U };
    MbPixel pixels[16];
    size_t index;

    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_movingblockshq_decode_data4x4(
              &reader, &predictor, pixels, 4U, NULL) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 44U);
    CHECK(predictor.luma == 7U);
    for (index = 0U; index < 16U; ++index) {
        CHECK(pixels[index].y == 7U);
        CHECK(pixels[index].u == 3U);
        CHECK(pixels[index].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_data2x2_wrap(void)
{
    /* opcode 2, U=0, V=0, then residuals 1,0,0,0. */
    static const uint8_t payload[] = { 0x02U, 0x70U, 0x15U };
    ReplayBitReader reader;
    MbPredictor predictor = { 31U };
    MbPixel pixels[4];

    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_movingblockshq_decode_data2x2(
              &reader, &predictor, pixels, 2U, NULL) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 21U);
    CHECK(predictor.luma == 0U);
    CHECK(pixels[0].y == 0U && pixels[1].y == 0U);
    CHECK(pixels[2].y == 0U && pixels[3].y == 0U);
    return EXIT_SUCCESS;
}

static int test_table_and_truncation(void)
{
    static const uint8_t truncated[] = { 0x8dU };
    ReplayBitReader reader;
    MbPredictor predictor = { 0U };
    MbPixel pixels[16];
    MbVerifyError error = { 0 };

    CHECK(mb_huffman_validate(&codec_movingblockshq_luma_huffman) ==
          REPLAY_OK);
    replay_bitreader_init(&reader, truncated, sizeof(truncated));
    CHECK(codec_movingblockshq_decode_data4x4(
              &reader, &predictor, pixels, 4U, &error) ==
          REPLAY_TRUNCATED_INPUT);
    CHECK(error.detail != NULL);
    return EXIT_SUCCESS;
}

static int test_complete_data_frame(void)
{
    static const uint8_t payload[] = {
        0x8dU, 0xa2U, 0xaaU, 0xaaU, 0xaaU, 0x0aU
    };
    MbPixel decoded_pixels[16];
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    size_t bits_consumed = 0U;
    size_t index;

    CHECK(codec_movingblockshq_verify_frame(
              payload, sizeof(payload), NULL, &decoded,
              &bits_consumed, NULL) == REPLAY_OK);
    CHECK(bits_consumed == 44U);
    for (index = 0U; index < 16U; ++index) {
        CHECK(decoded_pixels[index].y == 0U);
        CHECK(decoded_pixels[index].u == 3U);
        CHECK(decoded_pixels[index].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_4x4_copy_modes(void)
{
    MbPixel previous_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbMotionVector motion = { -1, 0, 0 };
    unsigned row;

    fill_previous(previous_pixels, 32U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);

    /* First parent is stationary; second copies a temporal offset. */
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 2U, 2U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_4X4, &motion) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 4U; ++row) {
        unsigned column;

        for (column = 0U; column < 4U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 8U + column],
                             &previous_pixels[row * 8U + column]));
            CHECK(same_pixel(&decoded_pixels[row * 8U + 4U + column],
                             &previous_pixels[row * 8U + 3U + column]));
        }
    }

    /* Rebuild the second parent as a spatial copy of the first. */
    replay_buffer_clear(&payload);
    replay_bitwriter_init(&writer, &payload);
    motion.dx = -4;
    motion.dy = 0;
    motion.spatial = 1;
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 2U, 2U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_4X4, &motion) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 4U; ++row) {
        unsigned column;

        for (column = 0U; column < 4U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 8U + 4U + column],
                             &decoded_pixels[row * 8U + column]));
        }
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_split_copy_modes(void)
{
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbMotionVector temporal = { -1, 0, 0 };
    MbMotionVector spatial = { -2, 0, 1 };
    unsigned row;

    fill_previous(previous_pixels, 16U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);

    CHECK(replay_bitwriter_write(&writer, 3U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_2X2, &temporal) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_2X2, &spatial) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 2U; ++row) {
        unsigned column;

        for (column = 0U; column < 2U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 4U + column],
                             &previous_pixels[row * 4U + column]));
            CHECK(same_pixel(&decoded_pixels[row * 4U + 2U + column],
                             &previous_pixels[row * 4U + 1U + column]));
            CHECK(same_pixel(&decoded_pixels[(row + 2U) * 4U + column],
                             &previous_pixels[(row + 2U) * 4U + column]));
            CHECK(same_pixel(
                &decoded_pixels[(row + 2U) * 4U + 2U + column],
                &decoded_pixels[(row + 2U) * 4U + column]));
        }
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data4x4_zero_residual() == EXIT_SUCCESS);
    CHECK(test_data2x2_wrap() == EXIT_SUCCESS);
    CHECK(test_table_and_truncation() == EXIT_SUCCESS);
    CHECK(test_complete_data_frame() == EXIT_SUCCESS);
    CHECK(test_4x4_copy_modes() == EXIT_SUCCESS);
    CHECK(test_split_copy_modes() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
