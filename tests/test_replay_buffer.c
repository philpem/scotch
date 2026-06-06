#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/replay_buffer.h"

int main(void)
{
    ReplayBuffer buffer;
    uint8_t source[200];
    size_t i;

    replay_buffer_init(&buffer);
    CHECK(buffer.data == NULL);
    CHECK(buffer.size == 0U);

    for (i = 0; i < sizeof(source); ++i) {
        source[i] = (uint8_t)i;
    }
    CHECK(replay_buffer_append(&buffer, source, sizeof(source)) == REPLAY_OK);
    CHECK(buffer.size == sizeof(source));
    CHECK(buffer.capacity >= buffer.size);
    CHECK(memcmp(buffer.data, source, sizeof(source)) == 0);

    replay_buffer_truncate(&buffer, 17U);
    CHECK(buffer.size == 17U);
    CHECK(replay_buffer_append_u8(&buffer, UINT8_C(0xa5)) == REPLAY_OK);
    CHECK(buffer.data[17] == UINT8_C(0xa5));

    replay_buffer_clear(&buffer);
    CHECK(buffer.size == 0U);
    CHECK(buffer.capacity >= sizeof(source));

    CHECK(replay_buffer_append(&buffer, NULL, 0U) == REPLAY_OK);
    CHECK(replay_buffer_append(&buffer, NULL, 1U) == REPLAY_INVALID_ARGUMENT);

    replay_buffer_free(&buffer);
    CHECK(buffer.data == NULL);
    CHECK(buffer.capacity == 0U);
    return EXIT_SUCCESS;
}

