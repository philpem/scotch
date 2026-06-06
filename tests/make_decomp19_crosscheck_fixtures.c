#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"

/*
 * Build small, mode-specific payloads for the compiled Acorn decoder test.
 * The source pixels are already in decoder-visible 6Y5UV form: RGB conversion
 * is deliberately outside this test so a failure identifies the bitstream or
 * decoder contract rather than colour quantisation.
 */

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

static int write_frame(const char *directory, const char *name,
                       const MbFrame *frame)
{
    uint8_t packed[8U * 4U * 3U];
    size_t offset = 0U;
    unsigned y;

    CHECK(frame->width * frame->height <= 8U * 4U);
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

static void copy_2x2(MbPixel *destination, size_t destination_stride,
                     unsigned destination_x, unsigned destination_y,
                     const MbPixel *source, size_t source_stride,
                     unsigned source_x, unsigned source_y)
{
    unsigned y;

    for (y = 0U; y < 2U; ++y) {
        unsigned x;
        for (x = 0U; x < 2U; ++x) {
            destination[(destination_y + y) * destination_stride +
                        destination_x + x] =
                source[(source_y + y) * source_stride + source_x + x];
        }
    }
}

static int encode_fixture(const char *directory, const char *stem,
                          const MbFrame *source, const MbFrame *previous,
                          const CodecSuperMovingBlocksEncodeOptions *options,
                          CodecSuperMovingBlocksEncodeStats *stats,
                          MbFrame *reconstructed)
{
    char name[128];
    ReplayBuffer payload;
    ReplayStatus status;

    replay_buffer_init(&payload);
    status = codec_supermovingblocks_encode_frame(
        source, previous, options, &payload, reconstructed, stats);
    CHECK(status == REPLAY_OK);

    CHECK(snprintf(name, sizeof(name), "%s.mb19", stem) > 0);
    CHECK(write_bytes(directory, name, payload.data, payload.size) ==
          EXIT_SUCCESS);
    CHECK(snprintf(name, sizeof(name), "%s.expected.6y5uv", stem) > 0);
    CHECK(write_frame(directory, name, reconstructed) == EXIT_SUCCESS);
    if (previous != NULL) {
        CHECK(snprintf(name, sizeof(name), "%s.previous.6y5uv", stem) > 0);
        CHECK(write_frame(directory, name, previous) == EXIT_SUCCESS);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int make_temporal4x4(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 1, 1, 0, 0, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    unsigned y;

    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 0U; x < 8U; ++x) {
            previous_pixels[y * 8U + x] =
                (MbPixel){ (uint8_t)(y * 8U + x), 4U, 6U };
            source_pixels[y * 8U + x] = previous_pixels[y * 8U + x];
        }
        for (x = 0U; x < 4U; ++x) {
            source_pixels[y * 8U + x] = previous_pixels[y * 8U + x + 1U];
        }
    }
    CHECK(encode_fixture(directory, "temporal4x4", &source, &previous,
                         &options, &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.temporal4x4_blocks == 1U);
    CHECK(stats.stationary4x4_blocks == 1U);
    return EXIT_SUCCESS;
}

static int make_spatial4x4(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 0, 1, 0, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    unsigned y;

    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 0U; x < 4U; ++x) {
            MbPixel pixel = { (uint8_t)(y * 4U + x), 5U, 27U };
            source_pixels[y * 8U + x] = pixel;
            source_pixels[y * 8U + x + 4U] = pixel;
        }
    }
    CHECK(encode_fixture(directory, "spatial4x4", &source, NULL, &options,
                         &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(stats.spatial4x4_blocks == 1U);
    return EXIT_SUCCESS;
}

static int make_temporal2x2(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 1, 0, 1, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    size_t i;

    for (i = 0U; i < 32U; ++i) {
        previous_pixels[i] =
            (MbPixel){ (uint8_t)((i * 11U + 7U) & 63U), 4U, 6U };
        source_pixels[i] =
            (MbPixel){ (uint8_t)((i * 17U + 3U) & 63U), 4U, 6U };
    }
    copy_2x2(source_pixels, 8U, 0U, 0U, previous_pixels, 8U, 1U, 0U);
    copy_2x2(source_pixels, 8U, 2U, 0U, previous_pixels, 8U, 0U, 0U);
    copy_2x2(source_pixels, 8U, 0U, 2U, previous_pixels, 8U, 1U, 1U);
    copy_2x2(source_pixels, 8U, 2U, 2U, previous_pixels, 8U, 0U, 1U);

    CHECK(encode_fixture(directory, "temporal2x2", &source, &previous,
                         &options, &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(stats.temporal2x2_blocks >= 4U);
    return EXIT_SUCCESS;
}

static int make_spatial2x2(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel reconstructed_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, reconstructed_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 0, 0, 1, 1, 0U };
    CodecSuperMovingBlocksEncodeStats stats;
    size_t i;

    for (i = 0U; i < 32U; ++i) {
        source_pixels[i] =
            (MbPixel){ (uint8_t)((i * 13U + 5U) & 63U), 7U, 9U };
    }
    copy_2x2(source_pixels, 8U, 4U, 0U, source_pixels, 8U, 2U, 0U);
    copy_2x2(source_pixels, 8U, 6U, 0U, source_pixels, 8U, 4U, 0U);
    copy_2x2(source_pixels, 8U, 4U, 2U, source_pixels, 8U, 6U, 0U);
    copy_2x2(source_pixels, 8U, 6U, 2U, source_pixels, 8U, 4U, 0U);

    CHECK(encode_fixture(directory, "spatial2x2", &source, NULL, &options,
                         &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.split4x4_blocks >= 1U);
    CHECK(stats.spatial2x2_blocks >= 4U);
    return EXIT_SUCCESS;
}

static int make_lossy_split(const char *directory)
{
    MbPixel source_pixels[16];
    MbPixel previous_pixels[16];
    MbPixel reconstructed_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame reconstructed = { 4U, 4U, 4U, reconstructed_pixels };
    CodecSuperMovingBlocksEncodeOptions options = { 1, 0, 0, 1, 7U };
    CodecSuperMovingBlocksEncodeStats stats;
    size_t i;

    for (i = 0U; i < 16U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)i, 3U, 6U };
        source_pixels[i] = previous_pixels[i];
    }
    source_pixels[0].y = (uint8_t)(source_pixels[0].y + 2U);
    source_pixels[10].y = 47U;
    source_pixels[11].y = 47U;
    source_pixels[14].y = 47U;
    source_pixels[15].y = 47U;

    CHECK(encode_fixture(directory, "lossy-split", &source, &previous,
                         &options, &stats, &reconstructed) == EXIT_SUCCESS);
    CHECK(stats.split4x4_blocks == 1U);
    CHECK(stats.stationary2x2_blocks == 3U);
    CHECK(stats.data2x2_blocks == 1U);
    CHECK(reconstructed_pixels[0].y == previous_pixels[0].y);
    CHECK(reconstructed_pixels[0].y != source_pixels[0].y);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(make_temporal4x4(argv[1]) == EXIT_SUCCESS);
    CHECK(make_spatial4x4(argv[1]) == EXIT_SUCCESS);
    CHECK(make_temporal2x2(argv[1]) == EXIT_SUCCESS);
    CHECK(make_spatial2x2(argv[1]) == EXIT_SUCCESS);
    CHECK(make_lossy_split(argv[1]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
