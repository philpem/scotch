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

/* A move opcode is not yet implemented; it must fail cleanly, not misdecode. */
static int test_move_rejected(void)
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
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, &error) ==
          REPLAY_MALFORMED_STREAM);
    CHECK(error.detail != NULL);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data4x4() == EXIT_SUCCESS);
    CHECK(test_split_data() == EXIT_SUCCESS);
    CHECK(test_move_rejected() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
