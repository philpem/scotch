#include "test_common.h"

#include <stdint.h>

#include "replay/mb_motion.h"
#include "replay/replay_buffer.h"

static int decode_motion(uint32_t family, unsigned index_bits,
                         uint32_t index, MbMotionBlockSize block_size,
                         MbMotionVector *motion)
{
    ReplayBuffer buffer;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    ReplayStatus status;

    replay_buffer_init(&buffer);
    replay_bitwriter_init(&writer, &buffer);
    status = replay_bitwriter_write(&writer, family, 2U);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_write(&writer, index, index_bits);
    }
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    if (status == REPLAY_OK) {
        replay_bitreader_init(&reader, buffer.data, buffer.size);
        status = mb_motion_read_format19(&reader, block_size, motion);
    }
    replay_buffer_free(&buffer);
    return status;
}

static int round_trip_motion(int dx, int dy, int spatial,
                             MbMotionBlockSize block_size)
{
    ReplayBuffer buffer;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    MbMotionVector expected = { dx, dy, spatial };
    MbMotionVector actual;

    replay_buffer_init(&buffer);
    replay_bitwriter_init(&writer, &buffer);
    CHECK(mb_motion_write_format19(&writer, block_size, &expected) ==
          REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    replay_bitreader_init(&reader, buffer.data, buffer.size);
    CHECK(mb_motion_read_format19(&reader, block_size, &actual) == REPLAY_OK);
    CHECK(actual.dx == dx && actual.dy == dy && actual.spatial == spatial);
    replay_buffer_free(&buffer);
    return EXIT_SUCCESS;
}

/* Write a type 7 vector and read it back, checking the move code round-trips. */
static int round_trip_format7(const MbMotionVector *expected,
                              MbMotionBlockSize block_size)
{
    ReplayBuffer buffer;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    MbMotionVector actual;

    replay_buffer_init(&buffer);
    replay_bitwriter_init(&writer, &buffer);
    CHECK(mb_motion_write_format7(&writer, block_size, expected) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    replay_bitreader_init(&reader, buffer.data, buffer.size);
    CHECK(mb_motion_read_format7(&reader, block_size, &actual) == REPLAY_OK);
    CHECK(actual.dx == expected->dx && actual.dy == expected->dy &&
          actual.spatial == expected->spatial);
    replay_buffer_free(&buffer);
    return EXIT_SUCCESS;
}

/* Every enumerated type 7 vector, plus stationary, must survive write->read. */
static int test_format7_round_trip(void)
{
    MbMotionVector stationary = { 0, 0, 0 };
    MbMotionBlockSize sizes[2] = { MB_MOTION_BLOCK_4X4, MB_MOTION_BLOCK_2X2 };
    unsigned s;
    unsigned index;

    for (s = 0U; s < 2U; ++s) {
        CHECK(round_trip_format7(&stationary, sizes[s]) == EXIT_SUCCESS);
        for (index = 0U; index < 80U; ++index) {
            MbMotionVector motion;

            CHECK(mb_motion_format7_temporal_at(index, &motion) == REPLAY_OK);
            CHECK(round_trip_format7(&motion, sizes[s]) == EXIT_SUCCESS);
        }
        for (index = 0U; index < 8U; ++index) {
            MbMotionVector motion;

            CHECK(mb_motion_format7_spatial_at(sizes[s], index, &motion) ==
                  REPLAY_OK);
            CHECK(round_trip_format7(&motion, sizes[s]) == EXIT_SUCCESS);
        }
    }
    CHECK(mb_motion_format7_temporal_at(80U, &stationary) ==
          REPLAY_INVALID_ARGUMENT);
    return EXIT_SUCCESS;
}

/*
 * Every enumerated format-19 temporal and spatial vector must survive
 * write->read at both block sizes. Because the reader maps each code to a
 * unique vector, a successful round-trip over the whole enumerated space proves
 * mb_motion_write_format19 emits the exact index the reader expects -- i.e. it
 * exhaustively exercises the O(1) ring_index/far_index inverses.
 */
static int test_format19_round_trip(void)
{
    MbMotionBlockSize sizes[2] = { MB_MOTION_BLOCK_4X4, MB_MOTION_BLOCK_2X2 };
    unsigned s;
    unsigned index;

    for (s = 0U; s < 2U; ++s) {
        for (index = 0U; index < 288U; ++index) {
            MbMotionVector v;

            CHECK(mb_motion_format19_temporal_at(index, &v) == REPLAY_OK);
            CHECK(round_trip_motion(v.dx, v.dy, 0, sizes[s]) == EXIT_SUCCESS);
        }
        for (index = 0U; index < 8U; ++index) {
            MbMotionVector v;

            CHECK(mb_motion_format19_spatial_at(sizes[s], index, &v) ==
                  REPLAY_OK);
            CHECK(round_trip_motion(v.dx, v.dy, v.spatial, sizes[s]) ==
                  EXIT_SUCCESS);
        }
    }
    {
        MbMotionVector v;

        CHECK(mb_motion_format19_temporal_at(288U, &v) ==
              REPLAY_INVALID_ARGUMENT);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    MbMotionVector motion;

    CHECK(decode_motion(0U, 3U, 4U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == 1 && motion.dy == 0 && !motion.spatial);

    CHECK(decode_motion(2U, 4U, 0U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == -2 && motion.dy == -2 && !motion.spatial);

    CHECK(decode_motion(1U, 5U, 5U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == -4 && motion.dy == 0 && motion.spatial);

    CHECK(decode_motion(1U, 5U, 5U, MB_MOTION_BLOCK_2X2, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == -2 && motion.dy == -1 && motion.spatial);

    CHECK(decode_motion(1U, 5U, 8U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == -3 && motion.dy == -3 && !motion.spatial);

    CHECK(decode_motion(3U, 8U, 0U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == -8 && motion.dy == -8 && !motion.spatial);

    CHECK(decode_motion(3U, 8U, 239U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_OK);
    CHECK(motion.dx == 8 && motion.dy == 8 && !motion.spatial);

    CHECK(decode_motion(3U, 8U, 240U, MB_MOTION_BLOCK_4X4, &motion) ==
          REPLAY_MALFORMED_STREAM);

    CHECK(round_trip_motion(1, 0, 0, MB_MOTION_BLOCK_4X4) == EXIT_SUCCESS);
    CHECK(round_trip_motion(-2, 2, 0, MB_MOTION_BLOCK_4X4) == EXIT_SUCCESS);
    CHECK(round_trip_motion(3, -1, 0, MB_MOTION_BLOCK_4X4) == EXIT_SUCCESS);
    CHECK(round_trip_motion(8, 8, 0, MB_MOTION_BLOCK_4X4) == EXIT_SUCCESS);
    CHECK(round_trip_motion(-4, 0, 1, MB_MOTION_BLOCK_4X4) == EXIT_SUCCESS);
    CHECK(round_trip_motion(-2, -1, 1, MB_MOTION_BLOCK_2X2) == EXIT_SUCCESS);
    CHECK(mb_motion_format19_spatial_at(
              MB_MOTION_BLOCK_4X4, 5U, &motion) == REPLAY_OK);
    CHECK(motion.dx == -4 && motion.dy == 0 && motion.spatial);
    CHECK(mb_motion_format19_spatial_at(
              MB_MOTION_BLOCK_4X4, 8U, &motion) == REPLAY_INVALID_ARGUMENT);

    CHECK(test_format19_round_trip() == EXIT_SUCCESS);
    CHECK(test_format7_round_trip() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
