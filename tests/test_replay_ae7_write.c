#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/replay_ae7.h"
#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"

/*
 * Round-trip the writer through the existing parser: build a movie from
 * synthetic frame payloads and one sound track, then confirm the catalogue,
 * sector alignment, even/odd sizes, and that every frame and the sound stream
 * are byte-exact recoverable from the declared chunk regions.
 */

#define WIDTH 16U
#define HEIGHT 16U
#define FPS 12.5
#define FRAME_COUNT 30U

int main(void)
{
    /* Distinct, varying-length frame payloads so chunk video sizes differ and
     * odd-length frames exercise the even-padding path. */
    static uint8_t frames[FRAME_COUNT][64];
    const uint8_t *frame_ptrs[FRAME_COUNT];
    size_t frame_sizes[FRAME_COUNT];
    /* 11025 Hz mono 8-bit over 30 frames at 12.5 fps = 2.4 s -> 26460 bytes. */
    static uint8_t sound[26460];
    ReplayAe7WriteTrack track;
    ReplayAe7WriteOptions options;
    ReplayBuffer movie_bytes;
    ReplayAe7Movie movie;
    char error[256];
    ReplayStatus status;
    size_t i;
    size_t cumulative_frames = 0U;
    uint64_t cumulative_sound = 0U;

    for (i = 0U; i < FRAME_COUNT; ++i) {
        size_t length = 33U + (i % 7U) * 3U; /* 33..51, mixes odd and even */
        size_t j;

        for (j = 0U; j < length; ++j) {
            frames[i][j] = (uint8_t)(i * 7U + j);
        }
        frame_ptrs[i] = frames[i];
        frame_sizes[i] = length;
    }
    for (i = 0U; i < sizeof(sound); ++i) {
        sound[i] = (uint8_t)(i * 3U + 1U);
    }

    memset(&track, 0, sizeof(track));
    track.codec = REPLAY_AE7_SOUND_VIDC_LOG;
    track.rate_hz = 11025U;
    track.channels = 1U;
    track.precision_bits = 8U;
    track.data = sound;
    track.size = sizeof(sound);

    memset(&options, 0, sizeof(options));
    options.title = "Round Trip";
    options.copyright = "(c) test";
    options.author = "writer test";
    options.video_codec = 19U;
    options.width = WIDTH;
    options.height = HEIGHT;
    options.pixel_depth = 16U;
    options.pixel_label = "6Y5UV";
    options.frames_per_second = FPS;
    options.frame_data = frame_ptrs;
    options.frame_size = frame_sizes;
    options.frame_count = FRAME_COUNT;
    options.chunk_seconds = 1.0; /* 12.5 fps -> alternating 12/13 frame chunks */
    options.tracks = &track;
    options.track_count = 1U;

    replay_buffer_init(&movie_bytes);
    status = replay_ae7_write(&options, &movie_bytes, error, sizeof(error));
    CHECK(status == REPLAY_OK);

    status = replay_ae7_parse(movie_bytes.data, movie_bytes.size, &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_OK);

    /* Header metadata survives the round trip. */
    CHECK(strcmp(movie.title, "Round Trip") == 0);
    CHECK(movie.video_codec == 19U);
    CHECK(movie.width == WIDTH);
    CHECK(movie.height == HEIGHT);
    CHECK(movie.frames_per_second == FPS);
    CHECK(movie.sound_codec == REPLAY_AE7_SOUND_VIDC_LOG);
    CHECK(movie.sound_rate == 11025U);
    CHECK(movie.sound_channels == 1U);
    CHECK(movie.key_frame_offset == -1);

    /* 12.5 fps, 1.0 s chunks over 30 frames -> 12,13,5 = three chunks. */
    CHECK(movie.chunk_count == 3U);
    /* Audio movies must never be a single chunk (the player aliases its two
     * chunk buffers and corrupts a one-chunk movie that has sound). */
    CHECK(movie.chunk_count >= 2U);

    {
        uint64_t even_expected = 0U;
        uint64_t odd_expected = 0U;
        unsigned align_mask = REPLAY_AE7_DEFAULT_ALIGN_MASK;

        for (i = 0U; i < movie.chunk_count; ++i) {
            const ReplayAe7Chunk *chunk = &movie.chunks[i];
            uint64_t total = chunk->video_bytes + chunk->sound_bytes;
            size_t frame;
            size_t expected_frames = (i == 0U) ? 12U : (i == 1U ? 13U : 5U);
            uint64_t reconstructed_video = 0U;
            uint64_t pos;

            /* Every chunk begins on a sector boundary. */
            CHECK((chunk->file_offset & (uint64_t)align_mask) == 0U);
            /* Video region length is even (halfword aligned). */
            CHECK((chunk->video_bytes & 1U) == 0U);
            CHECK(chunk->sound_tracks == 1U);

            /* The video region holds exactly this chunk's frames, in order. */
            pos = chunk->file_offset;
            for (frame = 0U; frame < expected_frames; ++frame) {
                size_t global = cumulative_frames + frame;
                CHECK(memcmp(&movie_bytes.data[pos], frame_ptrs[global],
                             frame_sizes[global]) == 0);
                pos += (uint64_t)frame_sizes[global];
                reconstructed_video += (uint64_t)frame_sizes[global];
            }
            /* Declared video_bytes equals the frames plus at most one pad. */
            CHECK(chunk->video_bytes >= reconstructed_video);
            CHECK(chunk->video_bytes - reconstructed_video <= 1U);

            /* The sound region holds the next slice of the continuous track. */
            CHECK(memcmp(&movie_bytes.data[chunk->file_offset +
                                           chunk->video_bytes],
                         &sound[cumulative_sound],
                         (size_t)chunk->sound_bytes) == 0);

            cumulative_frames += expected_frames;
            cumulative_sound += chunk->sound_bytes;

            if ((i & 1U) == 0U) {
                if (total > even_expected) {
                    even_expected = total;
                }
            } else {
                if (total > odd_expected) {
                    odd_expected = total;
                }
            }
        }
        CHECK(cumulative_frames == FRAME_COUNT);
        /* even/odd chunk size = max payload of that parity + one guard byte. */
        CHECK(movie.even_chunk_bytes == even_expected + 1U);
        CHECK(movie.odd_chunk_bytes == odd_expected + 1U);
        /* Audio is sliced by time and sums to (at most) the whole track. */
        CHECK(cumulative_sound <= (uint64_t)sizeof(sound));
        CHECK((uint64_t)sizeof(sound) - cumulative_sound < 11025U);
    }

    replay_ae7_movie_destroy(&movie);
    replay_buffer_free(&movie_bytes);

    /* A request that would yield a single chunk (frames_per_chunk == frames)
     * must still produce two chunks when there is sound. */
    options.frames_per_chunk = FRAME_COUNT; /* would be one chunk */
    options.chunk_seconds = 0.0;
    replay_buffer_init(&movie_bytes);
    status = replay_ae7_write(&options, &movie_bytes, error, sizeof(error));
    CHECK(status == REPLAY_OK);
    status = replay_ae7_parse(movie_bytes.data, movie_bytes.size, &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_OK);
    CHECK(movie.chunk_count == 2U);
    replay_ae7_movie_destroy(&movie);
    replay_buffer_free(&movie_bytes);

    /* Without sound the same request is free to be a single chunk. */
    options.track_count = 0U;
    replay_buffer_init(&movie_bytes);
    status = replay_ae7_write(&options, &movie_bytes, error, sizeof(error));
    CHECK(status == REPLAY_OK);
    status = replay_ae7_parse(movie_bytes.data, movie_bytes.size, &movie,
                              error, sizeof(error));
    CHECK(status == REPLAY_OK);
    CHECK(movie.chunk_count == 1U);
    replay_ae7_movie_destroy(&movie);
    replay_buffer_free(&movie_bytes);

    /* An embedded sprite is placed after the header with a non-zero offset and
     * its exact bytes are recoverable; without one the offset stays zero. */
    {
        static const uint8_t sprite_blob[40] = {
            1, 0, 0, 0, 16, 0, 0, 0, 44, 0, 0, 0 /* minimal area header */
        };
        options.sprite_data = sprite_blob;
        options.sprite_size = sizeof(sprite_blob);
        replay_buffer_init(&movie_bytes);
        status = replay_ae7_write(&options, &movie_bytes, error, sizeof(error));
        CHECK(status == REPLAY_OK);
        status = replay_ae7_parse(movie_bytes.data, movie_bytes.size, &movie,
                                  error, sizeof(error));
        CHECK(status == REPLAY_OK);
        CHECK(movie.sprite_offset != 0U);
        CHECK(movie.sprite_bytes == sizeof(sprite_blob));
        CHECK(memcmp(&movie_bytes.data[movie.sprite_offset], sprite_blob,
                     sizeof(sprite_blob)) == 0);
        /* The catalogue follows the sprite, not the bare header. */
        CHECK(movie.catalogue_offset >=
              movie.sprite_offset + sizeof(sprite_blob));
        replay_ae7_movie_destroy(&movie);
        replay_buffer_free(&movie_bytes);
    }

    /* Key frames: one block per chunk except the first, each the per-frame blob
     * of that chunk's last frame. 30 frames at 12.5 fps / 1 s -> chunks 12,13,5,
     * so two keys from frames 11 and 24. */
    {
        static uint8_t key_blobs[FRAME_COUNT][4];
        const uint8_t *key_ptrs[FRAME_COUNT];

        for (i = 0U; i < FRAME_COUNT; ++i) {
            key_blobs[i][0] = (uint8_t)i;
            key_ptrs[i] = key_blobs[i];
        }
        options.sprite_data = NULL;
        options.sprite_size = 0U;
        options.track_count = 0U;
        options.frames_per_chunk = 0U;
        options.chunk_seconds = 1.0;
        options.write_keys = 1;
        options.key_data = key_ptrs;
        options.key_size = sizeof(key_blobs[0]);
        replay_buffer_init(&movie_bytes);
        status = replay_ae7_write(&options, &movie_bytes, error, sizeof(error));
        CHECK(status == REPLAY_OK);
        status = replay_ae7_parse(movie_bytes.data, movie_bytes.size, &movie,
                                  error, sizeof(error));
        CHECK(status == REPLAY_OK);
        CHECK(movie.chunk_count == 3U);
        CHECK(movie.key_frame_offset > 0);
        /* Two key blocks (chunk_count - 1). */
        CHECK(movie.catalogue_offset - (uint64_t)movie.key_frame_offset ==
              2U * sizeof(key_blobs[0]));
        /* Block 0 is frame 11 (end of chunk 0), block 1 is frame 24. */
        CHECK(movie_bytes.data[movie.key_frame_offset] == 11U);
        CHECK(movie_bytes.data[(size_t)movie.key_frame_offset +
                               sizeof(key_blobs[0])] == 24U);
        replay_ae7_movie_destroy(&movie);
        replay_buffer_free(&movie_bytes);
    }
    return EXIT_SUCCESS;
}
