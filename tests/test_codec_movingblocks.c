#include "test_common.h"

#include <stdint.h>

#include "replay/codec_movingblocks.h"

/* Build a type 7 4x4 data block (top-level `1`, then 16 Y, U, V) and check the
 * literal values decode straight through with the shared block chroma. */
static int test_data4x4(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel pixels[16];
    MbFrame decoded = { 4U, 4U, 4U, pixels };
    size_t consumed = 0U;
    unsigned i;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK); /* data 4x4 */
    for (i = 0U; i < 16U; ++i) {
        CHECK(replay_bitwriter_write(&writer, (i + 3U) & 31U, 5U) == REPLAY_OK);
    }
    CHECK(replay_bitwriter_write(&writer, 9U, 5U) == REPLAY_OK);  /* U */
    CHECK(replay_bitwriter_write(&writer, 21U, 5U) == REPLAY_OK); /* V */
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, &consumed, NULL) ==
          REPLAY_OK);
    CHECK(consumed == 1U + 16U * 5U + 5U + 5U); /* 91 bits */
    for (i = 0U; i < 16U; ++i) {
        CHECK(pixels[i].y == ((i + 3U) & 31U));
        CHECK(pixels[i].u == 9U && pixels[i].v == 21U);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* Build a 4x4 split (`01`) of four data 2x2 children with distinct chroma and
 * confirm the TL,TR,BL,BR placement. */
static int test_split_data(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel pixels[16];
    MbFrame decoded = { 4U, 4U, 4U, pixels };
    static const unsigned origin[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    unsigned child;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    /* Top-level `01` = read 0 then 1. */
    CHECK(replay_bitwriter_write(&writer, 0U, 1U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    for (child = 0U; child < 4U; ++child) {
        unsigned i;

        CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK); /* data */
        for (i = 0U; i < 4U; ++i) {
            CHECK(replay_bitwriter_write(&writer, (child * 4U + i) & 31U, 5U) ==
                  REPLAY_OK);
        }
        CHECK(replay_bitwriter_write(&writer, child + 1U, 5U) == REPLAY_OK);
        CHECK(replay_bitwriter_write(&writer, child + 8U, 5U) == REPLAY_OK);
    }
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    for (child = 0U; child < 4U; ++child) {
        unsigned i;
        for (i = 0U; i < 4U; ++i) {
            unsigned px = (origin[child][1] + i / 2U) * 4U +
                          origin[child][0] + i % 2U;

            CHECK(pixels[px].y == ((child * 4U + i) & 31U));
            CHECK(pixels[px].u == child + 1U && pixels[px].v == child + 8U);
        }
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* A temporal copy needs a previous frame; without one it must fail cleanly. */
static int test_temporal_needs_previous(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel pixels[16];
    MbFrame decoded = { 4U, 4U, 4U, pixels };
    MbVerifyError error = { 0 };

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, 0U, 1U) == REPLAY_OK); /* `00` move */
    CHECK(replay_bitwriter_write(&writer, 0U, 1U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK); /* `00` stat. */
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, &error) ==
          REPLAY_MALFORMED_STREAM);
    CHECK(error.detail != NULL);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* A 4x4 `00` stationary move copies the previous frame block byte-for-byte. */
static int test_temporal_stationary(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel previous_pixels[16];
    MbPixel pixels[16];
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame decoded = { 4U, 4U, 4U, pixels };
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)((i * 2U + 1U) & 31U),
                                        (uint8_t)((i + 4U) & 31U),
                                        (uint8_t)((i + 7U) & 31U) };
    }
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK); /* `00` move */
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK); /* `00` stat. */
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size,
                                          &previous, &decoded, NULL, NULL) ==
          REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        CHECK(pixels[i].y == previous_pixels[i].y);
        CHECK(pixels[i].u == previous_pixels[i].u);
        CHECK(pixels[i].v == previous_pixels[i].v);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* A 4x4 spatial copy reproduces an already-decoded block from the same frame:
 * an 8x4 frame whose second block copies the first via (-4,0). */
static int test_spatial_copy(void)
{
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbPixel pixels[32];
    MbFrame decoded = { 8U, 4U, 8U, pixels };
    unsigned i;

    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    /* Block (0,0): literal data with per-pixel luma and shared chroma. */
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        CHECK(replay_bitwriter_write(&writer, (i + 5U) & 31U, 5U) == REPLAY_OK);
    }
    CHECK(replay_bitwriter_write(&writer, 6U, 5U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 13U, 5U) == REPLAY_OK);
    /* Block (4,0): spatial move `00`, family `11` index 56+5 = (-4,0). */
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 3U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 61U, 6U) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        unsigned row = i / 4U;
        unsigned col = i % 4U;
        const MbPixel *src = &pixels[row * 8U + col];
        const MbPixel *dst = &pixels[row * 8U + 4U + col];

        CHECK(dst->y == src->y && dst->u == src->u && dst->v == src->v);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data4x4() == EXIT_SUCCESS);
    CHECK(test_split_data() == EXIT_SUCCESS);
    CHECK(test_temporal_needs_previous() == EXIT_SUCCESS);
    CHECK(test_temporal_stationary() == EXIT_SUCCESS);
    CHECK(test_spatial_copy() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
