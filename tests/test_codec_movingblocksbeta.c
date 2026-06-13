#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_movingblocksbeta.h"
#include "replay/replay_bitstream.h"

/* Encode a frame, decode the stream it produced, and confirm the decode matches
 * the encoder's own reconstruction -- the encoder/decoder contract. */
static int encode_and_check(const MbFrame *source, const MbFrame *previous,
                            const CodecMovingBlocksBetaEncodeOptions *options,
                            MbFrame *reconstructed, MbPixel *decoded_pixels)
{
    ReplayBuffer payload;
    MbFrame decoded = { source->width, source->height, source->width,
                        decoded_pixels };
    size_t pixels = (size_t)source->width * source->height;

    replay_buffer_init(&payload);
    CHECK(codec_movingblocksbeta_encode_frame(source, previous, options,
                                              &payload, reconstructed,
                                              NULL) == REPLAY_OK);
    CHECK(codec_movingblocksbeta_verify_frame_variant(
              payload.data, payload.size, previous, &decoded, NULL, NULL,
              options->variant) == REPLAY_OK);
    CHECK(memcmp(decoded_pixels, reconstructed->pixels,
                 pixels * sizeof(MbPixel)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_encode_round_trip(CodecMovingBlocksBetaVariant variant)
{
    enum { W = 16U, H = 16U };
    MbPixel source_pixels[W * H];
    MbPixel recon_pixels[W * H];
    MbPixel previous_pixels[W * H];
    MbPixel decoded_pixels[W * H];
    MbFrame source = { W, H, W, source_pixels };
    MbFrame reconstructed = { W, H, W, recon_pixels };
    MbFrame previous = { W, H, W, previous_pixels };
    CodecMovingBlocksBetaEncodeOptions options;
    unsigned i;

    /* 6Y6UV working pixels: Y 0..63, U/V 0..63 (six-bit signed). */
    for (i = 0U; i < W * H; ++i) {
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 3U) & 63U),
                                      (uint8_t)((i / W) * 4U & 63U),
                                      (uint8_t)((i % W) * 4U & 63U) };
    }
    options = (CodecMovingBlocksBetaEncodeOptions){
        0, 0, 1, 1, 0U, MB_ENCODE_POLICY_LOWEST_ERROR, NULL, variant
    };
    CHECK(encode_and_check(&source, NULL, &options, &reconstructed,
                           decoded_pixels) == EXIT_SUCCESS);

    memcpy(previous_pixels, recon_pixels, sizeof(previous_pixels));
    for (i = 0U; i < W * H; i += 11U) {
        source_pixels[i].y = (uint8_t)((source_pixels[i].y + 7U) & 63U);
    }
    options.allow_stationary = 1;
    options.allow_temporal = 1;
    CHECK(encode_and_check(&source, &previous, &options, &reconstructed,
                           decoded_pixels) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_encode_round_trip(CODEC_MOVINGBLOCKSBETA_OLD) == EXIT_SUCCESS);
    CHECK(test_encode_round_trip(CODEC_MOVINGBLOCKSBETA_NEW) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
