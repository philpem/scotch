#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/replay_buffer.h"

static void fill_frame(MbPixel *pixels, size_t count, uint8_t base)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        pixels[i].y = (uint8_t)((base + i) & 63U);
        pixels[i].u = (uint8_t)((base + i) & 31U);
        pixels[i].v = (uint8_t)((base + i * 3U) & 31U);
    }
}

static int test_data_only_frame(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    uint8_t residuals[16] = { 0 };
    MbPixel pixels[32];
    MbFrame decoded = { 8, 4, 8, pixels };
    MbVerifyError error;
    size_t bits_consumed = 0;
    size_t i;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(codec_supermovingblocks_write_data4x4(
              &writer, 3U, 5U, residuals) == REPLAY_OK);
    CHECK(codec_supermovingblocks_write_data4x4(
              &writer, 7U, 9U, residuals) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    memset(pixels, 0xff, sizeof(pixels));
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              &bits_consumed, &error) == REPLAY_OK);
    CHECK(bits_consumed == 88U);
    for (i = 0; i < 32U; ++i) {
        CHECK(pixels[i].y == 0U);
        CHECK(pixels[i].u == (i % 8U < 4U ? 3U : 7U));
        CHECK(pixels[i].v == (i % 8U < 4U ? 5U : 9U));
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_split_data_frame(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    uint8_t residuals[4] = { 0 };
    MbPixel pixels[16];
    MbFrame decoded = { 4, 4, 4, pixels };
    size_t bits_consumed;
    unsigned block;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, UINT32_C(3), 2U) == REPLAY_OK);
    for (block = 0; block < 4U; ++block) {
        CHECK(codec_supermovingblocks_write_data2x2(
                  &writer, (uint8_t)(block + 1U),
                  (uint8_t)(block + 5U), residuals) == REPLAY_OK);
    }
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              &bits_consumed, NULL) == REPLAY_OK);
    CHECK(bits_consumed == 82U);
    CHECK(pixels[0].u == 1U && pixels[0].v == 5U);
    CHECK(pixels[2].u == 2U && pixels[2].v == 6U);
    CHECK(pixels[8].u == 3U && pixels[8].v == 7U);
    CHECK(pixels[10].u == 4U && pixels[10].v == 8U);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_stationary_modes(void)
{
    static const uint8_t stationary4x4[] = { UINT8_C(0x00) };
    static const uint8_t split_stationary[] = {
        UINT8_C(0x03), UINT8_C(0x00)
    };
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4, 4, 4, previous_pixels };
    MbFrame decoded = { 4, 4, 4, decoded_pixels };
    MbVerifyError error;
    size_t bits;

    fill_frame(previous_pixels, 16U, 7U);
    memset(decoded_pixels, 0, sizeof(decoded_pixels));
    CHECK(codec_supermovingblocks_verify_frame(
              stationary4x4, sizeof(stationary4x4), &previous, &decoded,
              &bits, &error) == REPLAY_OK);
    CHECK(bits == 2U);
    CHECK(memcmp(previous_pixels, decoded_pixels,
                 sizeof(previous_pixels)) == 0);

    memset(decoded_pixels, 0, sizeof(decoded_pixels));
    CHECK(codec_supermovingblocks_verify_frame(
              split_stationary, sizeof(split_stationary), &previous, &decoded,
              &bits, &error) == REPLAY_OK);
    CHECK(bits == 10U);
    CHECK(memcmp(previous_pixels, decoded_pixels,
                 sizeof(previous_pixels)) == 0);

    CHECK(codec_supermovingblocks_verify_frame(
              stationary4x4, sizeof(stationary4x4), NULL, &decoded,
              &bits, &error) == REPLAY_MALFORMED_STREAM);
    CHECK(error.block_x == 0U && error.block_y == 0U);
    CHECK(error.detail != NULL);
    return EXIT_SUCCESS;
}

static int write_motion(ReplayBitWriter *writer, unsigned prefix,
                        unsigned prefix_bits, unsigned family,
                        unsigned index, unsigned index_bits)
{
    if (replay_bitwriter_write(writer, prefix, prefix_bits) != REPLAY_OK ||
        replay_bitwriter_write(writer, family, 2U) != REPLAY_OK ||
        replay_bitwriter_write(writer, index, index_bits) != REPLAY_OK) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int test_temporal_4x4(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel previous_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame previous = { 8, 4, 8, previous_pixels };
    MbFrame decoded = { 8, 4, 8, decoded_pixels };
    size_t row;

    fill_frame(previous_pixels, 32U, 3U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(write_motion(&writer, 2U, 2U, 0U, 4U, 3U) == EXIT_SUCCESS);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0; row < 4U; ++row) {
        CHECK(memcmp(decoded_pixels + row * 8U,
                     previous_pixels + row * 8U + 1U,
                     4U * sizeof(MbPixel)) == 0);
        CHECK(memcmp(decoded_pixels + row * 8U + 4U,
                     previous_pixels + row * 8U + 4U,
                     4U * sizeof(MbPixel)) == 0);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_temporal_split_2x2(void)
{
    static const unsigned indices[4] = { 7U, 5U, 2U, 0U };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4, 4, 4, previous_pixels };
    MbFrame decoded = { 4, 4, 4, decoded_pixels };
    unsigned block;

    fill_frame(previous_pixels, 16U, 9U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, 3U, 2U) == REPLAY_OK);
    for (block = 0; block < 4U; ++block) {
        CHECK(write_motion(&writer, 1U, 1U, 0U, indices[block], 3U) ==
              EXIT_SUCCESS);
    }
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(decoded_pixels[0].y == previous_pixels[5].y);
    CHECK(decoded_pixels[2].y == previous_pixels[5].y);
    CHECK(decoded_pixels[8].y == previous_pixels[5].y);
    CHECK(decoded_pixels[10].y == previous_pixels[5].y);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_spatial_4x4(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel previous_pixels[64];
    MbPixel decoded_pixels[64];
    MbFrame previous = { 8, 8, 8, previous_pixels };
    MbFrame decoded = { 8, 8, 8, decoded_pixels };
    size_t row;

    fill_frame(previous_pixels, 64U, 11U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(write_motion(&writer, 2U, 2U, 1U, 5U, 5U) == EXIT_SUCCESS);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 4U; row < 8U; ++row) {
        CHECK(memcmp(decoded_pixels + row * 8U + 4U,
                     decoded_pixels + row * 8U,
                     4U * sizeof(MbPixel)) == 0);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_invalid_motion(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel pixels[16];
    MbFrame decoded = { 4, 4, 4, pixels };
    MbVerifyError error;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(write_motion(&writer, 2U, 2U, 0U, 0U, 3U) == EXIT_SUCCESS);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              NULL, &error) == REPLAY_MALFORMED_STREAM);
    CHECK(error.detail != NULL);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_malformed_payloads(void)
{
    static const uint8_t truncated_data[] = { UINT8_C(0x01) };
    static const uint8_t nonzero_padding[] = { UINT8_C(0x80) };
    static const uint8_t trailing_byte[] = { UINT8_C(0x00), UINT8_C(0x00) };
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4, 4, 4, previous_pixels };
    MbFrame decoded = { 4, 4, 4, decoded_pixels };
    MbVerifyError error;

    fill_frame(previous_pixels, 16U, 1U);
    CHECK(codec_supermovingblocks_verify_frame(
              NULL, 0U, &previous, &decoded,
              NULL, &error) == REPLAY_TRUNCATED_INPUT);
    CHECK(codec_supermovingblocks_verify_frame(
              truncated_data, sizeof(truncated_data), &previous, &decoded,
              NULL, &error) == REPLAY_TRUNCATED_INPUT);
    CHECK(codec_supermovingblocks_verify_frame(
              nonzero_padding, sizeof(nonzero_padding), &previous, &decoded,
              NULL, &error) == REPLAY_MALFORMED_STREAM);
    CHECK(codec_supermovingblocks_verify_frame(
              trailing_byte, sizeof(trailing_byte), &previous, &decoded,
              NULL, &error) == REPLAY_MALFORMED_STREAM);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data_only_frame() == EXIT_SUCCESS);
    CHECK(test_split_data_frame() == EXIT_SUCCESS);
    CHECK(test_stationary_modes() == EXIT_SUCCESS);
    CHECK(test_temporal_4x4() == EXIT_SUCCESS);
    CHECK(test_temporal_split_2x2() == EXIT_SUCCESS);
    CHECK(test_spatial_4x4() == EXIT_SUCCESS);
    CHECK(test_invalid_motion() == EXIT_SUCCESS);
    CHECK(test_malformed_payloads() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
