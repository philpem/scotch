#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/replay_type23.h"

int main(void)
{
    MbPixel source_pixels[4] = {
        { 1U, 2U, 30U }, { 2U, 4U, 0U },
        { 63U, 31U, 1U }, { 0U, 1U, 31U }
    };
    MbPixel decoded_pixels[4];
    MbFrame source = { 4U, 1U, 4U, source_pixels };
    MbFrame decoded = { 4U, 1U, 4U, decoded_pixels };
    uint8_t packed[6];
    static const uint8_t expected[6] = {
        0x81U, 0x30U, 0xfeU, 0x0fU, 0x00U, 0x00U
    };
    ReplayAe7Chunk chunk = { 0U, sizeof(packed), 0U, 0U };
    ReplayAe7Movie movie = { 0 };
    size_t bytes;
    size_t count;

    CHECK(replay_type23_frame_bytes(4U, 1U, &bytes) == REPLAY_OK);
    CHECK(bytes == sizeof(packed));
    CHECK(replay_type23_pack_frame(&source, packed, sizeof(packed)) ==
          REPLAY_OK);
    CHECK(memcmp(packed, expected, sizeof(expected)) == 0);

    movie.video_codec = 23U;
    movie.width = 4U;
    movie.height = 1U;
    movie.pixel_depth = 16U;
    movie.frames_per_chunk = 1U;
    movie.chunks = &chunk;
    movie.chunk_count = 1U;
    CHECK(replay_type23_frame_count(&movie, &count) == REPLAY_OK);
    CHECK(count == 1U);
    CHECK(replay_type23_unpack_frame(
              packed, sizeof(packed), &movie, 0U, &decoded) == REPLAY_OK);
    CHECK(decoded_pixels[0].y == 1U && decoded_pixels[1].y == 2U);
    CHECK(decoded_pixels[0].u == 3U && decoded_pixels[1].u == 3U);
    CHECK(decoded_pixels[0].v == 31U && decoded_pixels[1].v == 31U);
    CHECK(decoded_pixels[2].y == 63U && decoded_pixels[3].y == 0U);
    CHECK(decoded_pixels[2].u == 0U && decoded_pixels[3].u == 0U);
    CHECK(decoded_pixels[2].v == 0U && decoded_pixels[3].v == 0U);

    chunk.video_bytes = sizeof(packed) - 1U;
    CHECK(replay_type23_frame_count(&movie, &count) == REPLAY_MALFORMED_STREAM);
    return EXIT_SUCCESS;
}
