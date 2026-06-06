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
    return EXIT_SUCCESS;
}
