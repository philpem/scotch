#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/replay_ae7.h"
#include "replay/replay_buffer.h"
#include "replay/replay_type2.h"
#include "replay/replay_type23.h"

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-extract --input MOVIE --output-prefix PREFIX "
            "(--type2-layout type19-fields | "
            "--type23-layout 6y6y5u5v)\n");
}

static int read_file(const char *path, ReplayBuffer *buffer)
{
    uint8_t block[16384];
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(block, 1U, sizeof(block), file);

        if (count != 0U &&
            replay_buffer_append(buffer, block, count) != REPLAY_OK) {
            fprintf(stderr, "%s: unable to allocate input buffer\n", path);
            fclose(file);
            return EXIT_FAILURE;
        }
        if (count != sizeof(block)) {
            if (ferror(file)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
            break;
        }
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int write_frame(const char *prefix, size_t index, const MbFrame *frame)
{
    char path[4096];
    FILE *file;
    unsigned y;
    int length = snprintf(path, sizeof(path), "%s%06zu.6y5uv", prefix, index);

    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "generated frame path is too long\n");
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;
        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *pixel =
                &frame->pixels[(size_t)y * frame->stride + x];
            uint8_t packed[3] = { pixel->y, pixel->u, pixel->v };

            if (fwrite(packed, 1U, sizeof(packed), file) != sizeof(packed)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
        }
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_prefix = NULL;
    ReplayBuffer input;
    ReplayAe7Movie movie;
    MbPixel *pixels = NULL;
    char error[256];
    size_t count;
    size_t index;
    ReplayStatus status;
    int result = EXIT_FAILURE;
    int type19_fields = 0;
    int type23_422 = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--output-prefix") == 0 && i + 1 < argc) {
            output_prefix = argv[++i];
        } else if (strcmp(argv[i], "--type2-layout") == 0 && i + 1 < argc &&
                   strcmp(argv[i + 1], "type19-fields") == 0) {
            ++i;
            type19_fields = 1;
        } else if (strcmp(argv[i], "--type23-layout") == 0 &&
                   i + 1 < argc &&
                   strcmp(argv[i + 1], "6y6y5u5v") == 0) {
            ++i;
            type23_422 = 1;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (input_path == NULL || output_prefix == NULL ||
        type19_fields == type23_422) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    replay_buffer_init(&input);
    if (read_file(input_path, &input) != EXIT_SUCCESS) {
        goto done;
    }
    status = replay_ae7_parse(input.data, input.size, &movie,
                              error, sizeof(error));
    if (status != REPLAY_OK) {
        fprintf(stderr, "%s: %s: %s\n", input_path,
                replay_status_string(status), error);
        goto done;
    }
    status = type19_fields ? replay_type2_frame_count(&movie, &count)
                           : replay_type23_frame_count(&movie, &count);
    if (status != REPLAY_OK) {
        fprintf(stderr, "%s: unsupported selected layout: %s\n", input_path,
                replay_status_string(status));
        replay_ae7_movie_destroy(&movie);
        goto done;
    }
    if ((size_t)movie.width > SIZE_MAX / (size_t)movie.height ||
        (size_t)movie.width * movie.height > SIZE_MAX / sizeof(*pixels)) {
        fprintf(stderr, "%s: frame dimensions are too large\n", input_path);
        replay_ae7_movie_destroy(&movie);
        goto done;
    }
    pixels = malloc((size_t)movie.width * movie.height * sizeof(*pixels));
    if (pixels == NULL) {
        fprintf(stderr, "unable to allocate frame buffer\n");
        replay_ae7_movie_destroy(&movie);
        goto done;
    }
    for (index = 0U; index < count; ++index) {
        MbFrame frame = {
            movie.width, movie.height, movie.width, pixels
        };

        status = type19_fields
                     ? replay_type2_unpack_type19_fields(
                           input.data, input.size, &movie, index, &frame)
                     : replay_type23_unpack_frame(
                           input.data, input.size, &movie, index, &frame);
        if (status != REPLAY_OK ||
            write_frame(output_prefix, index, &frame) != EXIT_SUCCESS) {
            if (status != REPLAY_OK) {
                fprintf(stderr, "frame %zu: %s\n", index,
                        replay_status_string(status));
            }
            free(pixels);
            replay_ae7_movie_destroy(&movie);
            goto done;
        }
    }
    if (type19_fields) {
        printf("codec=2 name=\"16 bit colour uncompressed\" frames=%zu "
               "size=%ux%u layout=type19-fields\n",
               count, movie.width, movie.height);
    } else {
        printf("codec=23 name=\"6Y6Y5U5V packed 4:2:2\" frames=%zu "
               "size=%ux%u layout=6y6y5u5v\n",
               count, movie.width, movie.height);
    }
    free(pixels);
    replay_ae7_movie_destroy(&movie);
    result = EXIT_SUCCESS;

done:
    replay_buffer_free(&input);
    return result;
}
