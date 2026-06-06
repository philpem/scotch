#include "test_common.h"

#include <stdint.h>

#include "replay/replay_bitstream.h"

int main(void)
{
    ReplayBuffer buffer;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    uint32_t value;

    replay_buffer_init(&buffer);
    replay_bitwriter_init(&writer, &buffer);

    /* Bits: 101, then 110011, all emitted least-significant bit first. */
    CHECK(replay_bitwriter_write(&writer, UINT32_C(0x5), 3U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, UINT32_C(0x33), 6U) == REPLAY_OK);
    CHECK(replay_bitwriter_position(&writer) == 9U);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(buffer.size == 2U);
    CHECK(buffer.data[0] == UINT8_C(0x9d));
    CHECK(buffer.data[1] == UINT8_C(0x01));

    replay_bitreader_init(&reader, buffer.data, buffer.size);
    CHECK(replay_bitreader_read(&reader, 3U, &value) == REPLAY_OK);
    CHECK(value == UINT32_C(0x5));
    CHECK(replay_bitreader_read(&reader, 6U, &value) == REPLAY_OK);
    CHECK(value == UINT32_C(0x33));
    CHECK(replay_bitreader_position(&reader) == 9U);
    CHECK(replay_bitreader_read(&reader, 8U, &value) == REPLAY_TRUNCATED_INPUT);

    replay_buffer_clear(&buffer);
    replay_bitwriter_init(&writer, &buffer);
    CHECK(replay_bitwriter_write(&writer, UINT32_MAX, 32U) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(buffer.size == 4U);
    CHECK(buffer.data[0] == UINT8_C(0xff));
    CHECK(buffer.data[3] == UINT8_C(0xff));

    replay_buffer_free(&buffer);
    return EXIT_SUCCESS;
}

