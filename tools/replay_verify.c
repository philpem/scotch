#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/mb_codec.h"
#include "replay/replay_buffer.h"

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-verify --codec ID [--describe|--verify-huffman]\n");
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
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (codec == NULL || (!describe && !huffman)) {
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
    return huffman ? verify_huffman(codec) : EXIT_SUCCESS;
}
