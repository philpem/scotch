#include "test_common.h"

#include <stdint.h>

#include "replay/replay_type2.h"

int main(void)
{
    static const uint8_t file_data[] = {
        0x00U, 0x00U, 0x7fU, 0x00U, 0xc0U, 0xffU, 0x55U, 0xaaU,
        0x01U, 0x00U, 0x02U, 0x00U, 0x03U, 0x00U, 0x04U, 0x00U
    };
    ReplayAe7Chunk chunk = { 0U, sizeof(file_data), 0U, 0U };
    ReplayAe7Movie movie = { 0 };
    MbPixel pixels[4];
    MbFrame frame = { 2U, 2U, 2U, pixels };
    size_t count;

    movie.video_codec = 2U;
    movie.width = 2U;
    movie.height = 2U;
    movie.pixel_depth = 16U;
    movie.frames_per_chunk = 2U;
    movie.chunks = &chunk;
    movie.chunk_count = 1U;

    CHECK(replay_type2_frame_count(&movie, &count) == REPLAY_OK);
    CHECK(count == 2U);
    CHECK(replay_type2_unpack_type19_fields(
              file_data, sizeof(file_data), &movie, 0U, &frame) == REPLAY_OK);
    CHECK(pixels[0].y == 0U && pixels[0].u == 0U && pixels[0].v == 0U);
    CHECK(pixels[1].y == 63U && pixels[1].u == 1U && pixels[1].v == 0U);
    CHECK(pixels[2].y == 0U && pixels[2].u == 31U && pixels[2].v == 31U);
    CHECK(pixels[3].y == 21U && pixels[3].u == 9U && pixels[3].v == 21U);
    CHECK(replay_type2_unpack_type19_fields(
              file_data, sizeof(file_data), &movie, 2U, &frame) ==
          REPLAY_INVALID_ARGUMENT);

    chunk.video_bytes = sizeof(file_data) - 1U;
    CHECK(replay_type2_frame_count(&movie, &count) == REPLAY_MALFORMED_STREAM);
    return EXIT_SUCCESS;
}
