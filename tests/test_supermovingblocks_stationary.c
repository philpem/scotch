#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

int main(void)
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
        1, 0, 0, 0, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED
    };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayBuffer payload;
    size_t consumed_bits;
    size_t i;

    for (i = 0; i < 32U; ++i) {
        source_pixels[i].y = (uint8_t)(i & 63U);
        source_pixels[i].u = i % 8U < 4U ? 3U : 7U;
        source_pixels[i].v = i % 8U < 4U ? 5U : 9U;
        previous_pixels[i] = source_pixels[i];
    }
    /* Force the right block to be data-coded while leaving the left still. */
    source_pixels[4].y = 42U;

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              &consumed_bits, NULL) == REPLAY_OK);
    CHECK(consumed_bits == stats.bits_written);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    CHECK(memcmp(reconstructed_pixels, previous_pixels,
                 4U * sizeof(*previous_pixels)) == 0);

    memcpy(source_pixels, previous_pixels, sizeof(source_pixels));
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 2U);
    CHECK(stats.data4x4_blocks == 0U);
    CHECK(stats.bits_written == 4U);
    CHECK(payload.size == 1U && payload.data[0] == 0U);

    /* Level 7 permits four luma exceptions of two in a 4x4 candidate. */
    for (i = 0U; i < 4U; ++i) {
        source_pixels[i].y = (uint8_t)(previous_pixels[i].y + 2U);
    }
    options.loss_level = 7U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 2U);
    CHECK(stats.data4x4_blocks == 0U);
    CHECK(memcmp(reconstructed_pixels, previous_pixels,
                 sizeof(previous_pixels)) == 0);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              &consumed_bits, NULL) == REPLAY_OK);
    CHECK(memcmp(decoded_pixels, reconstructed_pixels,
                 sizeof(decoded_pixels)) == 0);

    /* A fifth exception exceeds that row's explicit 4x4 allowance. */
    source_pixels[8].y = (uint8_t)(previous_pixels[8].y + 2U);
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.data4x4_blocks == 1U);

    options.loss_level = 29U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_INVALID_ARGUMENT);
    CHECK(payload.size == 0U);

    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
