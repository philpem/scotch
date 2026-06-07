#include "replay/replay_type2.h"

#include <limits.h>

static ReplayStatus frame_bytes(const ReplayAe7Movie *movie, size_t *bytes)
{
    size_t pixels;

    if (movie == NULL || bytes == NULL || movie->video_codec != 2U ||
        movie->pixel_depth != 16U || movie->width == 0U ||
        movie->height == 0U || movie->frames_per_chunk == 0U ||
        (size_t)movie->width > SIZE_MAX / (size_t)movie->height) {
        return REPLAY_INVALID_ARGUMENT;
    }
    pixels = (size_t)movie->width * movie->height;
    if (pixels > SIZE_MAX / 2U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    *bytes = pixels * 2U;
    return REPLAY_OK;
}

ReplayStatus replay_type2_frame_count(const ReplayAe7Movie *movie,
                                      size_t *frame_count)
{
    size_t bytes;
    size_t total = 0U;
    size_t chunk_index;
    ReplayStatus status = frame_bytes(movie, &bytes);

    if (status != REPLAY_OK || frame_count == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (chunk_index = 0U; chunk_index < movie->chunk_count; ++chunk_index) {
        uint64_t video_bytes = movie->chunks[chunk_index].video_bytes;
        uint64_t frames;

        if (video_bytes % bytes != 0U) {
            return REPLAY_MALFORMED_STREAM;
        }
        frames = video_bytes / bytes;
        if (frames > movie->frames_per_chunk || frames > SIZE_MAX - total) {
            return REPLAY_MALFORMED_STREAM;
        }
        total += (size_t)frames;
    }
    *frame_count = total;
    return REPLAY_OK;
}

ReplayStatus replay_type2_unpack_type19_fields(
    const uint8_t *file_data, size_t file_size, const ReplayAe7Movie *movie,
    size_t frame_index, MbFrame *output)
{
    size_t bytes;
    size_t remaining = frame_index;
    size_t chunk_index;
    uint64_t frame_offset = 0U;
    unsigned y;
    ReplayStatus status = frame_bytes(movie, &bytes);

    if (status != REPLAY_OK || file_data == NULL || output == NULL ||
        output->pixels == NULL || output->width != movie->width ||
        output->height != movie->height || output->stride < output->width) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (chunk_index = 0U; chunk_index < movie->chunk_count; ++chunk_index) {
        const ReplayAe7Chunk *chunk = &movie->chunks[chunk_index];
        uint64_t frames;

        if (chunk->video_bytes % bytes != 0U) {
            return REPLAY_MALFORMED_STREAM;
        }
        frames = chunk->video_bytes / bytes;
        if (frames > movie->frames_per_chunk) {
            return REPLAY_MALFORMED_STREAM;
        }
        if ((uint64_t)remaining < frames) {
            if ((uint64_t)remaining > UINT64_MAX / bytes ||
                chunk->file_offset > UINT64_MAX - (uint64_t)remaining * bytes) {
                return REPLAY_MALFORMED_STREAM;
            }
            frame_offset = chunk->file_offset + (uint64_t)remaining * bytes;
            break;
        }
        if (frames > SIZE_MAX || remaining < (size_t)frames) {
            return REPLAY_MALFORMED_STREAM;
        }
        remaining -= (size_t)frames;
    }
    if (chunk_index == movie->chunk_count) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (frame_offset > file_size || bytes > file_size - (size_t)frame_offset) {
        return REPLAY_TRUNCATED_INPUT;
    }

    for (y = 0U; y < output->height; ++y) {
        unsigned x;

        for (x = 0U; x < output->width; ++x) {
            size_t pixel_index = (size_t)y * output->width + x;
            size_t source = (size_t)frame_offset + pixel_index * 2U;
            uint16_t word = (uint16_t)file_data[source] |
                            (uint16_t)((uint16_t)file_data[source + 1U] << 8U);
            MbPixel *pixel = &output->pixels[(size_t)y * output->stride + x];

            pixel->y = (uint8_t)(word & UINT16_C(0x003f));
            pixel->u = (uint8_t)((word >> 6U) & UINT16_C(0x001f));
            pixel->v = (uint8_t)((word >> 11U) & UINT16_C(0x001f));
        }
    }
    return REPLAY_OK;
}
