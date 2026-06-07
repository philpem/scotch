#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

int main(void)
{
    MbPixel source_pixels[16];
    MbPixel previous_pixels[16];
    MbPixel reconstructed_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame reconstructed = { 4U, 4U, 4U, reconstructed_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    CodecSuperMovingBlocksEncodeOptions options = {
        1, 0, 0, 1, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED
    };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayBuffer payload;
    size_t i;

    replay_buffer_init(&payload);
    for (i = 0U; i < 16U; ++i) {
        previous_pixels[i].y = (uint8_t)i;
        previous_pixels[i].u = 3U;
        previous_pixels[i].v = 6U;
        source_pixels[i] = previous_pixels[i];
    }
    source_pixels[10].y = 47U;
    source_pixels[11].y = 47U;
    source_pixels[14].y = 47U;
    source_pixels[15].y = 47U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks == 1U);
    CHECK(stats.data2x2_blocks == 1U);
    CHECK(stats.stationary2x2_blocks == 3U);
    CHECK(stats.data4x4_blocks == 0U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    CHECK(memcmp(reconstructed_pixels, source_pixels,
                 sizeof(source_pixels)) == 0);

    /* The 2x2 table permits one level-7 exception of two per quadrant. */
    source_pixels[0].y = (uint8_t)(previous_pixels[0].y + 2U);
    options.loss_level = 7U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.split4x4_blocks == 1U);
    CHECK(stats.data2x2_blocks == 1U);
    CHECK(stats.stationary2x2_blocks == 3U);
    CHECK(reconstructed_pixels[0].y == previous_pixels[0].y);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
