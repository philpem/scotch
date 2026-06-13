#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/mb_repeat.h"
#include "replay/mb_frame.h"
#include "replay/codec_movingblocks.h"
#include "replay/codec_movingblockshq.h"
#include "replay/codec_supermovingblocks.h"
#include "replay/codec_movingblocksbeta.h"

typedef ReplayStatus (*VerifyFn)(const uint8_t *, size_t, const MbFrame *,
                                 MbFrame *, size_t *, MbVerifyError *);

/* The repeat payload must (a) be the right length of zero bytes for the codec's
 * stationary-block bit width, and (b) decode through our verifier -- itself
 * cross-checked against the real Decomp module -- to exactly the previous
 * frame. */
static int check_codec(unsigned codec, VerifyFn verify, unsigned block_bits)
{
    enum { W = 12U, H = 8U };
    MbPixel previous_pixels[W * H];
    MbPixel decoded_pixels[W * H];
    MbFrame previous = { W, H, W, previous_pixels };
    MbFrame decoded = { W, H, W, decoded_pixels };
    ReplayBuffer payload;
    unsigned blocks = ((W + 3U) / 4U) * ((H + 3U) / 4U);
    size_t expected = ((size_t)blocks * block_bits + 7U) / 8U;
    unsigned i;

    for (i = 0U; i < W * H; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)(i & 31U),
                                        (uint8_t)((i * 2U) & 31U),
                                        (uint8_t)((i * 3U) & 31U) };
    }

    replay_buffer_init(&payload);
    CHECK(mb_repeat_payload(codec, W, H, &payload) == REPLAY_OK);
    CHECK(payload.size == expected);
    for (i = 0U; i < payload.size; ++i) {
        CHECK(payload.data[i] == 0U);
    }
    CHECK(verify(payload.data, payload.size, &previous, &decoded, NULL, NULL) ==
          REPLAY_OK);
    CHECK(memcmp(decoded_pixels, previous_pixels, sizeof(previous_pixels)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    ReplayBuffer payload;

    CHECK(check_codec(7U, codec_movingblocks_verify_frame, 4U) == EXIT_SUCCESS);
    CHECK(check_codec(17U, codec_movingblockshq_verify_frame, 2U) ==
          EXIT_SUCCESS);
    CHECK(check_codec(19U, codec_supermovingblocks_verify_frame, 2U) ==
          EXIT_SUCCESS);
    CHECK(check_codec(20U, codec_movingblocksbeta_verify_frame, 2U) ==
          EXIT_SUCCESS);

    /* Unknown codecs and zero dimensions are rejected. */
    replay_buffer_init(&payload);
    CHECK(mb_repeat_payload(2U, 16U, 16U, &payload) == REPLAY_UNSUPPORTED_CODEC);
    CHECK(mb_repeat_payload(19U, 0U, 16U, &payload) == REPLAY_INVALID_ARGUMENT);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
