#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

static void copy_2x2(MbPixel *destination, size_t destination_stride,
                     unsigned destination_x, unsigned destination_y,
                     const MbPixel *source, size_t source_stride,
                     unsigned source_x, unsigned source_y)
{
    unsigned row;

    for (row = 0U; row < 2U; ++row) {
        unsigned column;
        for (column = 0U; column < 2U; ++column) {
            destination[(destination_y + row) * destination_stride +
                        destination_x + column] =
                source[(source_y + row) * source_stride + source_x + column];
        }
    }
}

static int test_temporal_2x2(void)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 1, 0, 1, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayBuffer payload;
    size_t i;

    for (i = 0U; i < 32U; ++i) {
        previous_pixels[i].y = (uint8_t)((i * 11U + 7U) & 63U);
        previous_pixels[i].u = 4U;
        previous_pixels[i].v = 6U;
        source_pixels[i].y = (uint8_t)((i * 17U + 3U) & 63U);
        source_pixels[i].u = 4U;
        source_pixels[i].v = 6U;
    }
    copy_2x2(source_pixels, 8U, 0U, 0U, previous_pixels, 8U, 1U, 0U);
    copy_2x2(source_pixels, 8U, 2U, 0U, previous_pixels, 8U, 0U, 0U);
    copy_2x2(source_pixels, 8U, 0U, 2U, previous_pixels, 8U, 1U, 1U);
    copy_2x2(source_pixels, 8U, 2U, 2U, previous_pixels, 8U, 0U, 1U);

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(stats.temporal2x2_blocks >= 4U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_spatial_2x2_key_frame(void)
{
    MbPixel source_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 0, 1, 1, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayBuffer payload;
    size_t i;

    for (i = 0U; i < 32U; ++i) {
        source_pixels[i].y = (uint8_t)((i * 13U + 5U) & 63U);
        source_pixels[i].u = 7U;
        source_pixels[i].v = 9U;
    }
    copy_2x2(source_pixels, 8U, 4U, 0U, source_pixels, 8U, 2U, 0U);
    copy_2x2(source_pixels, 8U, 6U, 0U, source_pixels, 8U, 4U, 0U);
    copy_2x2(source_pixels, 8U, 4U, 2U, source_pixels, 8U, 6U, 0U);
    copy_2x2(source_pixels, 8U, 6U, 2U, source_pixels, 8U, 4U, 0U);

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_encode_frame(
              &source, NULL, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(stats.spatial2x2_blocks >= 4U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_temporal_2x2() == EXIT_SUCCESS);
    CHECK(test_spatial_2x2_key_frame() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
