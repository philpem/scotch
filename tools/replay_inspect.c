#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "replay/mb_codec.h"
#include "replay/replay_ae7.h"
#include "replay/replay_buffer.h"

static int read_file(const char *path, ReplayBuffer *buffer)
{
    uint8_t bytes[16384];
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(bytes, 1U, sizeof(bytes), file);

        if (count != 0U &&
            replay_buffer_append(buffer, bytes, count) != REPLAY_OK) {
            fprintf(stderr, "%s: unable to allocate input buffer\n", path);
            fclose(file);
            return EXIT_FAILURE;
        }
        if (count != sizeof(bytes)) {
            if (ferror(file)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
            break;
        }
    }
    fclose(file);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    ReplayBuffer input;
    ReplayAe7Movie movie;
    const MbCodec *codec;
    char error[256];
    size_t index;
    ReplayStatus status;

    if (argc != 2) {
        fprintf(stderr, "usage: replay-inspect MOVIE\n");
        return EXIT_FAILURE;
    }
    replay_buffer_init(&input);
    if (read_file(argv[1], &input) != EXIT_SUCCESS) {
        replay_buffer_free(&input);
        return EXIT_FAILURE;
    }
    status = replay_ae7_parse(input.data, input.size, &movie,
                              error, sizeof(error));
    if (status != REPLAY_OK) {
        fprintf(stderr, "%s: %s: %s\n", argv[1],
                replay_status_string(status), error);
        replay_buffer_free(&input);
        return EXIT_FAILURE;
    }

    codec = mb_codec_find(movie.video_codec);
    printf("title: %s\n", movie.title);
    printf("copyright: %s\n", movie.copyright);
    printf("author: %s\n", movie.author);
    if (codec != NULL) {
        printf("video codec: %u (%s)\n", movie.video_codec, codec->name);
    } else {
        printf("video codec: %u (unknown)\n", movie.video_codec);
    }
    printf("video: %ux%u, %u bits, %.10g frames/s\n",
           movie.width, movie.height, movie.pixel_depth,
           movie.frames_per_second);
    printf("sound: codec %u, %u Hz, %u channels, %u bits\n",
           movie.sound_codec, movie.sound_rate, movie.sound_channels,
           movie.sound_precision);
    printf("chunks: %zu entries (last index %u), %u frames/chunk\n",
           movie.chunk_count, movie.last_chunk, movie.frames_per_chunk);
    printf("catalogue offset: %" PRIu64 "\n", movie.catalogue_offset);
    printf("sprite: offset=%" PRIu64 " bytes=%" PRIu64 "\n",
           movie.sprite_offset, movie.sprite_bytes);
    if (movie.key_frame_offset < 0) {
        printf("key-frame table: none\n");
    } else {
        printf("key-frame table offset: %" PRId64 "\n",
               movie.key_frame_offset);
    }
    for (index = 0U; index < movie.chunk_count; ++index) {
        const ReplayAe7Chunk *chunk = &movie.chunks[index];

        printf("chunk %zu: offset=%" PRIu64 " video=%" PRIu64
               " sound=%" PRIu64 " tracks=%u\n",
               index, chunk->file_offset, chunk->video_bytes,
               chunk->sound_bytes, chunk->sound_tracks);
    }

    replay_ae7_movie_destroy(&movie);
    replay_buffer_free(&input);
    return EXIT_SUCCESS;
}
