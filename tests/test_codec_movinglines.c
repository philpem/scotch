#include "test_common.h"

#include <string.h>

#include "replay/codec_movinglines.h"
#include "replay/replay_buffer.h"

/* Round-trip a frame: encode against `previous`, decode the result, and require
   the decode to reproduce `source` exactly and consume the whole payload. */
static int round_trip(const uint16_t *source, const uint16_t *previous,
                      unsigned width, unsigned height)
{
    ReplayBuffer payload;
    uint16_t decoded[256];
    size_t total = (size_t)width * height;
    size_t consumed = 0U;

    replay_buffer_init(&payload);
    CHECK(codec_movinglines_encode_frame(source, previous, width, height,
                                         &payload) == REPLAY_OK);
    CHECK(codec_movinglines_decode_frame(payload.data, payload.size, previous,
                                         decoded, width, height,
                                         &consumed) == REPLAY_OK);
    CHECK(consumed == payload.size);
    CHECK(memcmp(decoded, source, total * sizeof(*source)) == 0);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_round_trip(void)
{
    uint16_t a[64];
    uint16_t b[64];
    unsigned i;

    /* Key frame: a gradient (literals) with an embedded long identical run
       (repeats). previous == NULL. */
    for (i = 0U; i < 64U; ++i) {
        a[i] = (uint16_t)((i * 37U) & 0x7FFFU);
    }
    for (i = 20U; i < 50U; ++i) {
        a[i] = 0x1234U; /* a 30-pixel repeat */
    }
    CHECK(round_trip(a, NULL, 16U, 4U) == EXIT_SUCCESS);

    /* Inter frame: mostly equal to `a` (same-position copies) with a couple of
       changed spans. */
    memcpy(b, a, sizeof(b));
    b[0] = 0x7FFFU;
    for (i = 5U; i < 9U; ++i) {
        b[i] = (uint16_t)(0x100U + i);
    }
    b[63] = 0x0001U;
    CHECK(round_trip(b, a, 16U, 4U) == EXIT_SUCCESS);

    /* Degenerate: a whole frame of one value, and a whole frame equal to prev. */
    for (i = 0U; i < 64U; ++i) {
        a[i] = 0x2AAAU;
    }
    CHECK(round_trip(a, NULL, 8U, 8U) == EXIT_SUCCESS);
    CHECK(round_trip(a, a, 8U, 8U) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static void put(ReplayBuffer *b, unsigned word)
{
    (void)replay_buffer_append_u8(b, (uint8_t)(word & 0xFFU));
    (void)replay_buffer_append_u8(b, (uint8_t)((word >> 8U) & 0xFFU));
}

/* A temporal copy reading from the previous frame at a non-zero offset. */
static int test_temporal_copy(void)
{
    /* code 160 is grid (dx=0, dy=+1): offset = +width. width 8 -> +8. */
    /* code 127 is grid (dx=0, dy=-1): offset = -8. */
    uint16_t previous[16];
    uint16_t decoded[16];
    ReplayBuffer s;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        previous[i] = (uint16_t)(i + 1U);
    }
    replay_buffer_init(&s);
    put(&s, 1U + (160U << 7U) + ((8U - 2U) << 1U)); /* p0..7 = prev[8..15] */
    put(&s, 1U + (127U << 7U) + ((8U - 2U) << 1U)); /* p8..15 = prev[0..7] */
    put(&s, 1U + (0x1CCU << 7U));                   /* end of frame */
    CHECK(codec_movinglines_decode_frame(s.data, s.size, previous, decoded,
                                         8U, 2U, NULL) == REPLAY_OK);
    for (i = 0U; i < 8U; ++i) {
        CHECK(decoded[i] == previous[i + 8U]);
        CHECK(decoded[i + 8U] == previous[i]);
    }
    replay_buffer_free(&s);
    return EXIT_SUCCESS;
}

/* A spatial copy reading from an earlier row of the current frame. */
static int test_spatial_copy(void)
{
    /* code 0x120 + k, k = (dy+9)*19 + (dx+9). dy=-1, dx=0 -> k = 8*19 + 9 = 161,
       code = 0x120 + 161 = 0x1C1, offset = -width. */
    uint16_t decoded[16];
    ReplayBuffer s;
    unsigned i;
    unsigned code = 0x120U + (8U * 19U + 9U);

    replay_buffer_init(&s);
    /* Row 0: eight literals 100..107. */
    for (i = 0U; i < 8U; ++i) {
        put(&s, (100U + i) << 1U);
    }
    /* Row 1: spatial copy of the row above. */
    put(&s, 1U + (code << 7U) + ((8U - 2U) << 1U));
    put(&s, 1U + (0x1CCU << 7U));
    CHECK(codec_movinglines_decode_frame(s.data, s.size, NULL, decoded, 8U, 2U,
                                         NULL) == REPLAY_OK);
    for (i = 0U; i < 8U; ++i) {
        CHECK(decoded[i] == 100U + i);
        CHECK(decoded[i + 8U] == 100U + i);
    }
    replay_buffer_free(&s);
    return EXIT_SUCCESS;
}

/* The 0x1f long-literal run: bit-packed 15-bit pixels, then halfword realign. */
static int test_long_literal(void)
{
    uint16_t pixels[5] = { 0x0001U, 0x7FFFU, 0x2468U, 0x1357U, 0x4000U };
    uint16_t decoded[5];
    ReplayBuffer s;
    uint64_t acc = 0U;
    unsigned nbits = 0U;
    unsigned i;

    replay_buffer_init(&s);
    put(&s, 1U + (0x1FU << 11U) + ((5U - 1U) << 1U));
    /* Pack five 15-bit pixels LSB-first, flushing whole bytes. */
    for (i = 0U; i < 5U; ++i) {
        acc |= (uint64_t)pixels[i] << nbits;
        nbits += 15U;
        while (nbits >= 8U) {
            (void)replay_buffer_append_u8(&s, (uint8_t)(acc & 0xFFU));
            acc >>= 8U;
            nbits -= 8U;
        }
    }
    if (nbits != 0U) {
        (void)replay_buffer_append_u8(&s, (uint8_t)(acc & 0xFFU));
    }
    if ((s.size & 1U) != 0U) {
        (void)replay_buffer_append_u8(&s, 0U); /* realign to a halfword */
    }
    put(&s, 1U + (0x1CCU << 7U));
    CHECK(codec_movinglines_decode_frame(s.data, s.size, NULL, decoded, 5U, 1U,
                                         NULL) == REPLAY_OK);
    CHECK(memcmp(decoded, pixels, sizeof(pixels)) == 0);
    replay_buffer_free(&s);
    return EXIT_SUCCESS;
}

static int test_errors(void)
{
    uint16_t decoded[16];
    uint16_t bad[4] = { 0x8000U, 0U, 0U, 0U };
    ReplayBuffer s;

    /* Temporal copy with no previous frame. */
    replay_buffer_init(&s);
    put(&s, 1U + (160U << 7U) + ((8U - 2U) << 1U));
    put(&s, 1U + (0x1CCU << 7U));
    CHECK(codec_movinglines_decode_frame(s.data, s.size, NULL, decoded, 8U, 2U,
                                         NULL) == REPLAY_MALFORMED_STREAM);
    replay_buffer_clear(&s);

    /* Truncated: a command with no terminator. */
    put(&s, (5U << 1U));
    CHECK(codec_movinglines_decode_frame(s.data, s.size, NULL, decoded, 4U, 1U,
                                         NULL) == REPLAY_TRUNCATED_INPUT);
    replay_buffer_free(&s);

    /* Encoder rejects a pixel with bit 15 set. */
    {
        ReplayBuffer out;
        replay_buffer_init(&out);
        CHECK(codec_movinglines_encode_frame(bad, NULL, 4U, 1U, &out) ==
              REPLAY_INVALID_ARGUMENT);
        replay_buffer_free(&out);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_round_trip() == EXIT_SUCCESS);
    CHECK(test_temporal_copy() == EXIT_SUCCESS);
    CHECK(test_spatial_copy() == EXIT_SUCCESS);
    CHECK(test_long_literal() == EXIT_SUCCESS);
    CHECK(test_errors() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
