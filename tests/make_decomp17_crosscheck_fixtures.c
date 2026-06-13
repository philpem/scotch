#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "replay/codec_movingblockshq.h"

/*
 * Build type 17 data-frame payloads with our encoder and write the exact
 * YUV555 frame it reconstructs. The companion shell test decodes each payload
 * on the compiled Acorn Decomp17 and compares, proving the encoder's output is
 * decodable identically by real Acorn code. Source pixels are already 5-bit
 * YUV555 working values so a failure points at the bitstream, not colour
 * quantisation.
 */

#define MAX_PIXELS (16U * 16U)

static int write_bytes(const char *directory, const char *name,
                       const void *data, size_t size)
{
    char path[1024];
    FILE *file;
    int written;
    int result = EXIT_SUCCESS;

    written = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "fixture path is too long: %s/%s\n", directory, name);
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fwrite(data, 1U, size, file) != size) {
        perror(path);
        result = EXIT_FAILURE;
    }
    if (fclose(file) != 0) {
        perror(path);
        result = EXIT_FAILURE;
    }
    return result;
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

static int encode_fixture(const char *directory, const char *stem,
                          const MbFrame *source, MbFrame *reconstructed)
{
    char name[128];
    ReplayBuffer payload;
    size_t bits = 0U;
    ReplayStatus status;

    replay_buffer_init(&payload);
    status = codec_movingblockshq_encode_data_frame(
        source, &payload, reconstructed, &bits);
    CHECK(status == REPLAY_OK);
    CHECK(payload.size > 0U);

    CHECK(snprintf(name, sizeof(name), "%s.mb17", stem) > 0);
    CHECK(write_bytes(directory, name, payload.data, payload.size) ==
          EXIT_SUCCESS);
    CHECK(snprintf(name, sizeof(name), "%s.expected.yuv555", stem) > 0);
    CHECK(write_frame(directory, name, reconstructed) == EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int encode_frame_fixture(const char *directory, const char *stem,
                                const MbFrame *source, const MbFrame *previous,
                                const CodecMovingBlocksHqEncodeOptions *options,
                                CodecMovingBlocksHqEncodeStats *stats,
                                MbFrame *reconstructed)
{
    char name[128];
    ReplayBuffer payload;
    ReplayStatus status;

    replay_buffer_init(&payload);
    status = codec_movingblockshq_encode_frame(
        source, previous, options, &payload, reconstructed, stats);
    CHECK(status == REPLAY_OK);
    CHECK(payload.size > 0U);

    CHECK(snprintf(name, sizeof(name), "%s.mb17", stem) > 0);
    CHECK(write_bytes(directory, name, payload.data, payload.size) ==
          EXIT_SUCCESS);
    CHECK(snprintf(name, sizeof(name), "%s.expected.yuv555", stem) > 0);
    CHECK(write_frame(directory, name, reconstructed) == EXIT_SUCCESS);
    if (previous != NULL) {
        CHECK(snprintf(name, sizeof(name), "%s.previous.yuv555", stem) > 0);
        CHECK(write_frame(directory, name, previous) == EXIT_SUCCESS);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* Key frame: the right 4x4 duplicates the left, giving a data + spatial copy. */
static int make_spatial4x4(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecMovingBlocksHqEncodeOptions options = { 0, 0, 1, 0, 0U };
    CodecMovingBlocksHqEncodeStats stats;
    unsigned y;

    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 0U; x < 4U; ++x) {
            MbPixel pixel = { (uint8_t)((y * 4U + x) & 31U), 9U, 21U };
            source_pixels[y * 8U + x] = pixel;
            source_pixels[y * 8U + x + 4U] = pixel;
        }
    }
    CHECK(encode_frame_fixture(directory, "spatial4x4", &source, NULL, &options,
                               &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(stats.spatial4x4_blocks == 1U);
    return EXIT_SUCCESS;
}

/* Inter frame: the left 4x4 equals the previous frame (stationary copy); the
 * right differs and is coded as data. */
static int make_stationary4x4(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecMovingBlocksHqEncodeOptions options = { 1, 0, 0, 0, 0U };
    CodecMovingBlocksHqEncodeStats stats;
    unsigned i;

    for (i = 0U; i < 32U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)((i * 5U) & 31U), 4U, 6U };
        source_pixels[i] = previous_pixels[i];
    }
    for (i = 0U; i < 4U; ++i) {
        unsigned x;
        for (x = 4U; x < 8U; ++x) {
            source_pixels[i * 8U + x].y =
                (uint8_t)((source_pixels[i * 8U + x].y + 9U) & 31U);
        }
    }
    CHECK(encode_frame_fixture(directory, "stationary4x4", &source, &previous,
                               &options, &stats, &reconstructed) ==
          EXIT_SUCCESS);
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.data4x4_blocks == 1U);
    return EXIT_SUCCESS;
}

/* Inter frame: the left 4x4 copies the previous frame shifted one pixel right
 * (a temporal motion copy); the right 4x4 is an unchanged stationary copy. */
static int make_temporal4x4(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecMovingBlocksHqEncodeOptions options = { 1, 1, 0, 0, 0U };
    CodecMovingBlocksHqEncodeStats stats;
    unsigned y;

    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 0U; x < 8U; ++x) {
            previous_pixels[y * 8U + x] =
                (MbPixel){ (uint8_t)((y * 8U + x) & 31U), 4U, 6U };
            source_pixels[y * 8U + x] = previous_pixels[y * 8U + x];
        }
        for (x = 0U; x < 4U; ++x) {
            source_pixels[y * 8U + x] = previous_pixels[y * 8U + x + 1U];
        }
    }
    CHECK(encode_frame_fixture(directory, "temporal4x4", &source, &previous,
                               &options, &stats, &reconstructed) ==
          EXIT_SUCCESS);
    CHECK(stats.temporal4x4_blocks == 1U);
    CHECK(stats.stationary4x4_blocks == 1U);
    return EXIT_SUCCESS;
}

/* One 4x4 block with a luma ramp and distinct chroma. */
static int make_data4x4(const char *directory)
{
    MbPixel source_pixels[16];
    MbPixel reconstructed_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame reconstructed = { 4U, 4U, 4U, reconstructed_pixels };
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        source_pixels[i] =
            (MbPixel){ (uint8_t)(i & 31U), 3U, 5U };
    }
    return encode_fixture(directory, "data4x4", &source, &reconstructed);
}

/* Four 4x4 blocks so the cross-block predictor carry is exercised. */
static int make_data8x8(const char *directory)
{
    MbPixel source_pixels[64];
    MbPixel reconstructed_pixels[64];
    MbFrame source = { 8U, 8U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 8U, 8U, reconstructed_pixels };
    unsigned i;

    for (i = 0U; i < 64U; ++i) {
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 7U) & 31U),
                                      (uint8_t)((i * 3U + 1U) & 31U),
                                      (uint8_t)((i * 5U + 2U) & 31U) };
    }
    return encode_fixture(directory, "data8x8", &source, &reconstructed);
}

/* Adjacent extremes force large residuals that wrap modulo 32. */
static int make_data_wrap(const char *directory)
{
    MbPixel source_pixels[16];
    MbPixel reconstructed_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame reconstructed = { 4U, 4U, 4U, reconstructed_pixels };
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        source_pixels[i] =
            (MbPixel){ (uint8_t)((i & 1U) ? 31U : 0U), 17U, 30U };
    }
    return encode_fixture(directory, "data-wrap", &source, &reconstructed);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(make_data4x4(argv[1]) == EXIT_SUCCESS);
    CHECK(make_data8x8(argv[1]) == EXIT_SUCCESS);
    CHECK(make_data_wrap(argv[1]) == EXIT_SUCCESS);
    CHECK(make_spatial4x4(argv[1]) == EXIT_SUCCESS);
    CHECK(make_stationary4x4(argv[1]) == EXIT_SUCCESS);
    CHECK(make_temporal4x4(argv[1]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
