#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "replay/codec_movingblocks.h"
#include "replay/replay_bitstream.h"
#include "replay/replay_buffer.h"

/*
 * Build type 7 payloads with hand-laid literal blocks (there is no type 7
 * encoder yet), decode them with our verifier, and write each payload plus the
 * YUV555 frame it produced. The companion shell test decodes the same payloads
 * on the compiled Acorn Decomp7 and compares byte-for-byte, anchoring our
 * understanding of the literal-data format and block grammar.
 */

#define MAX_PIXELS (16U * 16U)

static int write_bytes(const char *directory, const char *name,
                       const void *data, size_t size)
{
    char path[1024];
    FILE *file;
    int written = snprintf(path, sizeof(path), "%s/%s", directory, name);

    if (written < 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "fixture path too long: %s/%s\n", directory, name);
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fwrite(data, 1U, size, file) != size || fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* Pack a frame as the harness emits yuv555: one Y, U, V byte per pixel. */
static int write_frame(const char *directory, const char *name,
                       const MbFrame *frame)
{
    uint8_t packed[MAX_PIXELS * 3U];
    size_t offset = 0U;
    unsigned y;

    CHECK((size_t)frame->width * frame->height <= MAX_PIXELS);
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;
        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *pixel = &frame->pixels[y * frame->stride + x];
            packed[offset++] = pixel->y;
            packed[offset++] = pixel->u;
            packed[offset++] = pixel->v;
        }
    }
    return write_bytes(directory, name, packed, offset);
}

/* Write a literal 4x4 data block: top-level `1`, sixteen Y, then U and V. */
static void put_data4x4(ReplayBitWriter *writer, const MbFrame *source,
                        unsigned x, unsigned y)
{
    unsigned i;

    replay_bitwriter_write(writer, 1U, 1U);
    for (i = 0U; i < 16U; ++i) {
        const MbPixel *p =
            &source->pixels[(size_t)(y + i / 4U) * source->stride + x + i % 4U];
        replay_bitwriter_write(writer, p->y, 5U);
    }
    replay_bitwriter_write(writer, source->pixels[(size_t)y * source->stride +
                                                  x].u, 5U);
    replay_bitwriter_write(writer, source->pixels[(size_t)y * source->stride +
                                                  x].v, 5U);
}

/* Write a literal 2x2 data child: sub-block `1`, four Y, then U and V. */
static void put_data2x2(ReplayBitWriter *writer, const MbFrame *source,
                        unsigned x, unsigned y)
{
    unsigned i;

    replay_bitwriter_write(writer, 1U, 1U);
    for (i = 0U; i < 4U; ++i) {
        const MbPixel *p =
            &source->pixels[(size_t)(y + i / 2U) * source->stride + x + i % 2U];
        replay_bitwriter_write(writer, p->y, 5U);
    }
    replay_bitwriter_write(writer, source->pixels[(size_t)y * source->stride +
                                                  x].u, 5U);
    replay_bitwriter_write(writer, source->pixels[(size_t)y * source->stride +
                                                  x].v, 5U);
}

static int emit_fixture(const char *directory, const char *stem,
                        const ReplayBuffer *payload, const MbFrame *expected)
{
    char name[128];

    CHECK(snprintf(name, sizeof(name), "%s.mb7", stem) > 0);
    CHECK(write_bytes(directory, name, payload->data, payload->size) ==
          EXIT_SUCCESS);
    CHECK(snprintf(name, sizeof(name), "%s.expected.yuv555", stem) > 0);
    CHECK(write_frame(directory, name, expected) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

/* An 8x8 frame of four literal 4x4 data blocks (per-block uniform chroma). Each
 * 90-bit block spans the four-word bitstream load, so this exercises the
 * unaligned multi-block read across block boundaries. */
static int make_data(const char *directory)
{
    MbPixel source_pixels[64];
    MbPixel decoded_pixels[64];
    MbFrame source = { 8U, 8U, 8U, source_pixels };
    MbFrame decoded = { 8U, 8U, 8U, decoded_pixels };
    ReplayBuffer payload;
    unsigned by;

    for (by = 0U; by < 8U; ++by) {
        unsigned bx;
        for (bx = 0U; bx < 8U; ++bx) {
            unsigned block = (by / 4U) * 2U + bx / 4U;
            source_pixels[by * 8U + bx] = (MbPixel){
                (uint8_t)((by * 8U + bx) & 31U),
                (uint8_t)((block * 7U + 1U) & 31U),
                (uint8_t)((block * 5U + 2U) & 31U)
            };
        }
    }
    replay_buffer_init(&payload);
    {
        ReplayBitWriter writer;
        replay_bitwriter_init(&writer, &payload);
        for (by = 0U; by < 8U; by += 4U) {
            unsigned bx;
            for (bx = 0U; bx < 8U; bx += 4U) {
                put_data4x4(&writer, &source, bx, by);
            }
        }
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    }
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    CHECK(emit_fixture(directory, "data", &payload, &decoded) == EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* A 4x4 split into four literal 2x2 data children. */
static int make_split(const char *directory)
{
    MbPixel source_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    static const unsigned origin[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    ReplayBuffer payload;
    unsigned i;
    unsigned child;

    for (i = 0U; i < 16U; ++i) {
        unsigned block = (i / 4U / 2U) * 2U + (i % 4U) / 2U;
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 3U) & 31U),
                                      (uint8_t)((block + 4U) & 31U),
                                      (uint8_t)((block + 9U) & 31U) };
    }
    replay_buffer_init(&payload);
    {
        ReplayBitWriter writer;
        replay_bitwriter_init(&writer, &payload);
        replay_bitwriter_write(&writer, 0U, 1U); /* `01` split */
        replay_bitwriter_write(&writer, 1U, 1U);
        for (child = 0U; child < 4U; ++child) {
            put_data2x2(&writer, &source, origin[child][0], origin[child][1]);
        }
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    }
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    CHECK(emit_fixture(directory, "split", &payload, &decoded) == EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(make_data(argv[1]) == EXIT_SUCCESS);
    CHECK(make_split(argv[1]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
