#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_codec.h"
#include "replay/replay_buffer.h"

/*
 * Verification is kept as a separate executable and decode path so encoder
 * bugs cannot be hidden by inspecting only encoder-owned reconstruction. Raw
 * temporal payloads need a caller-provided previous frame, which this simple
 * one-frame CLI deliberately does not synthesize.
 */
static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-verify --codec ID [--describe|--verify-huffman] "
            "[--payload FILE --size WIDTHxHEIGHT]\n");
}

static int read_file(const char *path, ReplayBuffer *buffer)
{
    uint8_t chunk[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(chunk, 1U, sizeof(chunk), file);
        if (count != 0U &&
            replay_buffer_append(buffer, chunk, count) != REPLAY_OK) {
            fprintf(stderr, "unable to allocate payload buffer\n");
            fclose(file);
            return EXIT_FAILURE;
        }
        if (count < sizeof(chunk)) {
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

static int verify_payload_file(const MbCodec *codec, const char *path,
                               unsigned width, unsigned height)
{
    ReplayBuffer payload;
    MbFrame frame;
    MbVerifyError error;
    size_t bits;
    ReplayStatus status;
    size_t pixel_count;

    if (codec->id != REPLAY_CODEC_SUPERMOVINGBLOCKS) {
        fprintf(stderr, "payload verification is not implemented for %s\n",
                codec->name);
        return EXIT_FAILURE;
    }
    if (width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        fprintf(stderr, "invalid frame dimensions\n");
        return EXIT_FAILURE;
    }
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(*frame.pixels)) {
        fprintf(stderr, "frame dimensions are too large\n");
        return EXIT_FAILURE;
    }

    replay_buffer_init(&payload);
    if (read_file(path, &payload) != EXIT_SUCCESS) {
        replay_buffer_free(&payload);
        return EXIT_FAILURE;
    }
    frame.width = width;
    frame.height = height;
    frame.stride = width;
    frame.pixels = calloc(pixel_count, sizeof(*frame.pixels));
    if (frame.pixels == NULL) {
        fprintf(stderr, "unable to allocate decoded frame\n");
        replay_buffer_free(&payload);
        return EXIT_FAILURE;
    }

    status = codec_supermovingblocks_verify_frame(
        payload.data, payload.size, NULL, &frame, &bits, &error);
    if (status == REPLAY_OK) {
        printf("codec=%u payload=\"%s\" size=%ux%u bits=%zu status=ok\n",
               (unsigned)codec->id, path, width, height, bits);
    } else {
        fprintf(stderr,
                "payload failed: %s bit=%zu block=%u,%u detail=%s\n",
                replay_status_string(status), error.bit_position,
                error.block_x, error.block_y,
                error.detail == NULL ? "unspecified" : error.detail);
    }
    free(frame.pixels);
    replay_buffer_free(&payload);
    return status == REPLAY_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int verify_huffman(const MbCodec *codec)
{
    unsigned expected;

    if (codec->luma_huffman == NULL) {
        fprintf(stderr, "%s has no implemented luma Huffman table\n",
                codec->name);
        return EXIT_FAILURE;
    }
    if (mb_huffman_validate(codec->luma_huffman) != REPLAY_OK) {
        fprintf(stderr, "%s luma Huffman table is invalid\n", codec->name);
        return EXIT_FAILURE;
    }

    for (expected = 0; expected < codec->luma_huffman->symbol_count;
         ++expected) {
        ReplayBuffer buffer;
        ReplayBitWriter writer;
        ReplayBitReader reader;
        unsigned actual;
        ReplayStatus status;

        replay_buffer_init(&buffer);
        replay_bitwriter_init(&writer, &buffer);
        status = mb_huffman_write(&writer, codec->luma_huffman, expected);
        if (status == REPLAY_OK) {
            status = replay_bitwriter_flush_zero(&writer);
        }
        if (status == REPLAY_OK) {
            replay_bitreader_init(&reader, buffer.data, buffer.size);
            status = mb_huffman_read(&reader, codec->luma_huffman, &actual);
        }
        replay_buffer_free(&buffer);

        if (status != REPLAY_OK || actual != expected) {
            fprintf(stderr, "symbol %u failed: %s\n", expected,
                    replay_status_string(status));
            return EXIT_FAILURE;
        }
    }

    printf("codec=%u name=\"%s\" huffman_symbols=%zu status=ok\n",
           (unsigned)codec->id, codec->name,
           codec->luma_huffman->symbol_count);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    const MbCodec *codec = NULL;
    int describe = 0;
    int huffman = 0;
    const char *payload_path = NULL;
    unsigned width = 0;
    unsigned height = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            char *end;
            unsigned long id = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || id > 999UL) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            codec = mb_codec_find((unsigned)id);
        } else if (strcmp(argv[i], "--describe") == 0) {
            describe = 1;
        } else if (strcmp(argv[i], "--verify-huffman") == 0) {
            huffman = 1;
        } else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            payload_path = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            char tail;
            if (sscanf(argv[++i], "%ux%u%c", &width, &height, &tail) != 2) {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (codec == NULL || (!describe && !huffman && payload_path == NULL) ||
        (payload_path != NULL && (width == 0U || height == 0U))) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (describe) {
        printf("codec=%u name=\"%s\" bits=%u,%u,%u block=%ux%u motion=%ux%u\n",
               (unsigned)codec->id, codec->name,
               codec->y_bits, codec->u_bits, codec->v_bits,
               codec->block_width, codec->block_height,
               codec->max_motion_x, codec->max_motion_y);
    }
    if (huffman && verify_huffman(codec) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return payload_path == NULL
               ? EXIT_SUCCESS
               : verify_payload_file(codec, payload_path, width, height);
}
