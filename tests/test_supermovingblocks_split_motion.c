#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_motion.h"

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
    CodecSuperMovingBlocksEncodeOptions options = {
        0, 1, 0, 1, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED
    };
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

    /*
     * Make the top-left child's stationary reconstruction acceptable but one
     * luma step worse than its exact temporal match. Other children retain
     * different motion so the parent cannot collapse to one 4x4 copy.
     */
    previous_pixels[0].y = 10U;
    previous_pixels[1].y = 11U;
    previous_pixels[2].y = 12U;
    previous_pixels[8].y = 20U;
    previous_pixels[9].y = 21U;
    previous_pixels[10].y = 22U;
    copy_2x2(source_pixels, 8U, 0U, 0U, previous_pixels, 8U, 1U, 0U);
    copy_2x2(source_pixels, 8U, 2U, 0U, previous_pixels, 8U, 0U, 0U);
    copy_2x2(source_pixels, 8U, 0U, 2U, previous_pixels, 8U, 1U, 1U);
    copy_2x2(source_pixels, 8U, 2U, 2U, previous_pixels, 8U, 0U, 1U);
    options.allow_stationary = 1;
    options.loss_level = 8U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(stats.stationary2x2_blocks >= 1U);
    CHECK(reconstructed_pixels[0].y != source_pixels[0].y);

    options.policy = CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(memcmp(reconstructed_pixels, source_pixels,
                 2U * sizeof(*source_pixels)) == 0);
    CHECK(memcmp(reconstructed_pixels + 8U, source_pixels + 8U,
                 2U * sizeof(*source_pixels)) == 0);
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
    CodecSuperMovingBlocksEncodeOptions options = {
        0, 0, 1, 1, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED
    };
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

static int test_spatial_2x2_rejects_future_parent(void)
{
    MbPixel decoded_pixels[64] = { { 0U, 0U, 0U } };
    MbFrame decoded = { 8U, 8U, 8U, decoded_pixels };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbMotionVector future = { 2, -2, 1 };
    MbVerifyError error;
    uint8_t residuals4x4[16] = { 0U };
    uint8_t residuals2x2[4] = { 0U };

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);

    /* Complete the two top-row parents, then enter parent (0,4). */
    CHECK(codec_supermovingblocks_write_data4x4(
              &writer, 0U, 0U, residuals4x4) == REPLAY_OK);
    CHECK(codec_supermovingblocks_write_data4x4(
              &writer, 0U, 0U, residuals4x4) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, UINT32_C(3), 2U) == REPLAY_OK);
    CHECK(codec_supermovingblocks_write_data2x2(
              &writer, 0U, 0U, residuals2x2) == REPLAY_OK);
    CHECK(codec_supermovingblocks_write_data2x2(
              &writer, 0U, 0U, residuals2x2) == REPLAY_OK);
    CHECK(codec_supermovingblocks_write_data2x2(
              &writer, 0U, 0U, residuals2x2) == REPLAY_OK);

    /*
     * Child (2,6) plus (2,-2) reads (4,4), which belongs to the next
     * top-level parent and has not been reconstructed. Older verification
     * accidentally read whatever happened to be in that output buffer.
     */
    CHECK(replay_bitwriter_write(&writer, UINT32_C(1), 1U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_2X2, &future) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              NULL, &error) == REPLAY_MALFORMED_STREAM);
    CHECK(error.block_x == 2U);
    CHECK(error.block_y == 6U);
    CHECK(strcmp(error.detail,
                 "spatial reference has not been reconstructed") == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_temporal_2x2() == EXIT_SUCCESS);
    CHECK(test_spatial_2x2_key_frame() == EXIT_SUCCESS);
    CHECK(test_spatial_2x2_rejects_future_parent() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
