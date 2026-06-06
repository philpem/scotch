#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

int main(void)
{
    MbPixel source_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 0, 1, 0, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayBuffer payload;
    size_t row;

    for (row = 0U; row < 4U; ++row) {
        size_t x;
        for (x = 0U; x < 4U; ++x) {
            MbPixel pixel = {
                (uint8_t)(row * 4U + x), 5U, 27U
            };
            source_pixels[row * 8U + x] = pixel;
            source_pixels[row * 8U + x + 4U] = pixel;
        }
    }

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_encode_frame(
              &source, NULL, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(stats.spatial4x4_blocks == 1U);
    CHECK(stats.stationary4x4_blocks == 0U);
    CHECK(stats.temporal4x4_blocks == 0U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    CHECK(memcmp(reconstructed_pixels, source_pixels,
                 sizeof(source_pixels)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
