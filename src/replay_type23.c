#include "replay/replay_type23.h"

#include <limits.h>
#include <string.h>

static int signed_chroma(uint8_t value)
{
    return value < 16U ? (int)value : (int)value - 32;
}

static uint8_t average_signed_chroma(uint8_t left, uint8_t right)
{
    int sum = signed_chroma(left) + signed_chroma(right);
    /* ARM ASR rounds a negative sum downward before division by two. */
    int average = sum >= 0 ? sum / 2 : -((-sum + 1) / 2);

    return (uint8_t)((unsigned)average & 31U);
}

ReplayStatus replay_type23_frame_bytes(unsigned width, unsigned height,
                                       size_t *bytes)
{
    size_t pixels;
    size_t bits;

    if (bytes == NULL || width == 0U || height == 0U || (width & 1U) != 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        return REPLAY_INVALID_ARGUMENT;
    }
    pixels = (size_t)width * height;
    if (pixels > (SIZE_MAX - 7U) / 11U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    bits = pixels * 11U;
    *bytes = (bits + 7U) / 8U;
    return REPLAY_OK;
}

static ReplayStatus validate_movie(const ReplayAe7Movie *movie, size_t *bytes)
{
    if (movie == NULL || movie->video_codec != 23U ||
        movie->pixel_depth != 16U || movie->frames_per_chunk == 0U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    return replay_type23_frame_bytes(movie->width, movie->height, bytes);
}

ReplayStatus replay_type23_frame_count(const ReplayAe7Movie *movie,
                                       size_t *frame_count)
{
    size_t bytes;
    size_t total = 0U;
    size_t chunk_index;
    ReplayStatus status = validate_movie(movie, &bytes);

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

static ReplayStatus frame_offset(const ReplayAe7Movie *movie, size_t bytes,
                                 size_t frame_index, uint64_t *offset)
{
    size_t remaining = frame_index;
    size_t chunk_index;

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
            *offset = chunk->file_offset + (uint64_t)remaining * bytes;
            return REPLAY_OK;
        }
        if (frames > SIZE_MAX || remaining < (size_t)frames) {
            return REPLAY_MALFORMED_STREAM;
        }
        remaining -= (size_t)frames;
    }
    return REPLAY_INVALID_ARGUMENT;
}

ReplayStatus replay_type23_unpack_frame(const uint8_t *file_data,
                                        size_t file_size,
                                        const ReplayAe7Movie *movie,
                                        size_t frame_index, MbFrame *output)
{
    size_t bytes;
    uint64_t offset;
    size_t pair_count;
    size_t pair;
    uint64_t bits = 0U;
    unsigned available = 0U;
    const uint8_t *source;
    size_t source_index = 0U;
    ReplayStatus status = validate_movie(movie, &bytes);

    if (status != REPLAY_OK || file_data == NULL || output == NULL ||
        output->pixels == NULL || output->width != movie->width ||
        output->height != movie->height || output->stride < output->width) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = frame_offset(movie, bytes, frame_index, &offset);
    if (status != REPLAY_OK) {
        return status;
    }
    if (offset > file_size || bytes > file_size - (size_t)offset) {
        return REPLAY_TRUNCATED_INPUT;
    }
    source = file_data + (size_t)offset;
    pair_count = (size_t)output->width * output->height / 2U;
    for (pair = 0U; pair < pair_count; ++pair) {
        uint32_t packed;
        size_t pixel_index = pair * 2U;
        unsigned y = (unsigned)(pixel_index / output->width);
        unsigned x = (unsigned)(pixel_index % output->width);
        MbPixel *left = &output->pixels[(size_t)y * output->stride + x];
        MbPixel *right = left + 1;

        while (available < 22U && source_index < bytes) {
            bits |= (uint64_t)source[source_index++] << available;
            available += 8U;
        }
        if (available < 22U) {
            return REPLAY_TRUNCATED_INPUT;
        }
        packed = (uint32_t)(bits & UINT32_C(0x003fffff));
        bits >>= 22U;
        available -= 22U;
        left->y = (uint8_t)(packed & 63U);
        right->y = (uint8_t)((packed >> 6U) & 63U);
        left->u = right->u = (uint8_t)((packed >> 12U) & 31U);
        left->v = right->v = (uint8_t)((packed >> 17U) & 31U);
    }
    return REPLAY_OK;
}

ReplayStatus replay_type23_pack_frame(const MbFrame *source,
                                      uint8_t *output, size_t output_size)
{
    size_t bytes;
    size_t pair_count;
    size_t pair;
    uint64_t bits = 0U;
    unsigned occupied = 0U;
    size_t output_index = 0U;
    ReplayStatus status;

    if (source == NULL || source->pixels == NULL ||
        source->stride < source->width || output == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = replay_type23_frame_bytes(source->width, source->height, &bytes);
    if (status != REPLAY_OK || output_size < bytes) {
        return REPLAY_INVALID_ARGUMENT;
    }
    memset(output, 0, bytes);
    pair_count = (size_t)source->width * source->height / 2U;
    for (pair = 0U; pair < pair_count; ++pair) {
        size_t pixel_index = pair * 2U;
        unsigned y = (unsigned)(pixel_index / source->width);
        unsigned x = (unsigned)(pixel_index % source->width);
        const MbPixel *left =
            &source->pixels[(size_t)y * source->stride + x];
        const MbPixel *right = left + 1;
        uint32_t packed;

        if (left->y > 63U || right->y > 63U || left->u > 31U ||
            right->u > 31U || left->v > 31U || right->v > 31U) {
            return REPLAY_INVALID_ARGUMENT;
        }
        packed = left->y | ((uint32_t)right->y << 6U) |
                 ((uint32_t)average_signed_chroma(left->u, right->u) << 12U) |
                 ((uint32_t)average_signed_chroma(left->v, right->v) << 17U);
        bits |= (uint64_t)packed << occupied;
        occupied += 22U;
        while (occupied >= 8U) {
            output[output_index++] = (uint8_t)bits;
            bits >>= 8U;
            occupied -= 8U;
        }
    }
    if (occupied != 0U) {
        output[output_index++] = (uint8_t)bits;
    }
    return output_index == bytes ? REPLAY_OK : REPLAY_INTERNAL_ERROR;
}
