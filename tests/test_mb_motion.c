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
    return EXIT_SUCCESS;
}
