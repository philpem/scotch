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
        1, 1, 0, 0, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED, NULL
    };
    CodecSuperMovingBlocksEncodeStats stats;
    CodecSuperMovingBlocksWorkspace workspace = { NULL };
    ReplayBuffer payload;
    size_t row;

    for (row = 0U; row < 4U; ++row) {
        size_t x;
        for (x = 0U; x < 8U; ++x) {
            previous_pixels[row * 8U + x].y =
                (uint8_t)(row * 8U + x);
            previous_pixels[row * 8U + x].u = 4U;
            previous_pixels[row * 8U + x].v = 6U;
        }
        for (x = 0U; x < 4U; ++x) {
            source_pixels[row * 8U + x] =
                previous_pixels[row * 8U + x + 1U];
        }
        for (x = 4U; x < 8U; ++x) {
            source_pixels[row * 8U + x] = previous_pixels[row * 8U + x];
        }
    }

    replay_buffer_init(&payload);
    CHECK(codec_supermovingblocks_workspace_init(
              &workspace, source.width, source.height) == REPLAY_OK);
    options.workspace = &workspace;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.temporal4x4_blocks == 1U);
    CHECK(stats.temporal4x4_evaluations != 0U);
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.data4x4_blocks == 0U);
    CHECK(stats.bits_written == 9U);
    CHECK(payload.size == 2U);
    CHECK(payload.data[0] == UINT8_C(0x42));
    CHECK(payload.data[1] == UINT8_C(0x00));
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);

    /*
     * At level 8 the left block's same-position error of one per pixel is
     * acceptable. Ordered policy therefore takes its two-bit stationary
     * representation, while lowest-error finds the exact (+1,0) temporal
     * reconstruction. The unchanged right block remains stationary in both.
     */
    options.loss_level = 8U;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 2U);
    CHECK(stats.temporal4x4_blocks == 0U);
    CHECK(memcmp(reconstructed_pixels, source_pixels,
                 sizeof(source_pixels)) != 0);

    options.policy = CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR;
    CHECK(codec_supermovingblocks_encode_frame(
              &source, &previous, &options, &payload, &reconstructed,
              &stats) == REPLAY_OK);
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.temporal4x4_blocks == 1U);
    /* The level-0 scan populated the same position for every quality row. */
    CHECK(stats.temporal4x4_evaluations == 0U);
    CHECK(memcmp(reconstructed_pixels, source_pixels,
                 sizeof(source_pixels)) == 0);
    CHECK(codec_supermovingblocks_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    CHECK(memcmp(reconstructed_pixels, decoded_pixels,
                 sizeof(decoded_pixels)) == 0);
    codec_supermovingblocks_workspace_destroy(&workspace);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
