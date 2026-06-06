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
    ReplayBuffer payload;
    size_t written_bits = 0U;
    size_t consumed_bits = 0U;
    size_t i;

    for (i = 0; i < 32U; ++i) {
        source_pixels[i].y = (uint8_t)((i * 7U + 3U) & 63U);
        source_pixels[i].u = i % 8U < 4U ? 31U : 6U;
        source_pixels[i].v = i % 8U < 4U ? 2U : 29U;
    }

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_encode_data_frame(
              &source, &payload, &reconstructed,
              &written_bits) == REPLAY_OK);
    CHECK(payload.size != 0U);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              &consumed_bits, NULL) == REPLAY_OK);
    CHECK(consumed_bits == written_bits);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    for (i = 0; i < 32U; ++i) {
        CHECK(reconstructed_pixels[i].y == source_pixels[i].y);
        CHECK(reconstructed_pixels[i].u == (i % 8U < 4U ? 31U : 6U));
        CHECK(reconstructed_pixels[i].v == (i % 8U < 4U ? 2U : 29U));
    }

    source_pixels[0].y = 64U;
    CHECK(codec_supermovingblocks_encode_data_frame(
              &source, &payload, &reconstructed, NULL) ==
          REPLAY_INVALID_ARGUMENT);
    CHECK(payload.size == 0U);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
