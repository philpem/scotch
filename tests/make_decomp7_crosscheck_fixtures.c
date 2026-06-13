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

/*
 * Write a move code: the family prefix (`00` stationary, `01`+3 radius-1,
 * `10`+4 radius-2, `11`+6 radius-4/3/spatial) least-significant bit first. The
 * caller has already written the move opcode (`00` top-level or `0` split).
 */
static void put_move_code(ReplayBitWriter *writer, unsigned family,
                          unsigned index)
{
    static const unsigned family_value[4] = { 0U, 2U, 1U, 3U };
    static const unsigned index_bits[4] = { 0U, 3U, 4U, 6U };

    replay_bitwriter_write(writer, family_value[family], 2U);
    if (index_bits[family] != 0U) {
        replay_bitwriter_write(writer, index, index_bits[family]);
    }
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

/*
 * Four 4x4 temporal copies of a gradient previous frame, one per move family:
 * stationary `00`, radius-2 (-2,0), radius-4 (0,-4) and radius-1 (-1,-1). The
 * gradient makes every motion vector resolve to distinct pixels, so a wrong
 * table entry shifts the copied block and the cross-check fails.
 */
static int make_temporal(const char *directory)
{
    MbPixel previous_pixels[64];
    MbPixel decoded_pixels[64];
    MbFrame previous = { 8U, 8U, 8U, previous_pixels };
    MbFrame decoded = { 8U, 8U, 8U, decoded_pixels };
    ReplayBuffer payload;
    unsigned i;

    for (i = 0U; i < 64U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)((i * 11U + 1U) & 31U),
                                        (uint8_t)((i * 3U + 5U) & 31U),
                                        (uint8_t)((i * 7U + 9U) & 31U) };
    }
    replay_buffer_init(&payload);
    {
        ReplayBitWriter writer;
        replay_bitwriter_init(&writer, &payload);
        /* (0,0) stationary. */
        replay_bitwriter_write(&writer, 0U, 2U);
        put_move_code(&writer, 0U, 0U);
        /* (4,0) radius-2 (-2,0): index 7 -> source (2,0). */
        replay_bitwriter_write(&writer, 0U, 2U);
        put_move_code(&writer, 2U, 7U);
        /* (0,4) radius-4 (0,-4): index 4 -> source (0,0). */
        replay_bitwriter_write(&writer, 0U, 2U);
        put_move_code(&writer, 3U, 4U);
        /* (4,4) radius-1 (-1,-1): index 0 -> source (3,3). */
        replay_bitwriter_write(&writer, 0U, 2U);
        put_move_code(&writer, 1U, 0U);
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    }
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size,
                                          &previous, &decoded, NULL,
                                          NULL) == REPLAY_OK);
    CHECK(write_frame(directory, "temporal.previous.yuv555", &previous) ==
          EXIT_SUCCESS);
    CHECK(emit_fixture(directory, "temporal", &payload, &decoded) ==
          EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/*
 * A literal 4x4 data block followed by a 4x4 spatial copy of it: the second
 * block uses the source-defined (-4,0) vector (4x4 spatial index 5), copying
 * from the reconstruction in progress rather than the previous frame.
 */
static int make_spatial(const char *directory)
{
    MbPixel source_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    ReplayBuffer payload;
    unsigned i;

    for (i = 0U; i < 32U; ++i) {
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 5U + 2U) & 31U),
                                      (uint8_t)((i + 6U) & 31U),
                                      (uint8_t)((i + 11U) & 31U) };
    }
    replay_buffer_init(&payload);
    {
        ReplayBitWriter writer;
        replay_bitwriter_init(&writer, &payload);
        put_data4x4(&writer, &source, 0U, 0U);
        /* (4,0) spatial (-4,0): top-level move, family 11 index 56+5. */
        replay_bitwriter_write(&writer, 0U, 2U);
        put_move_code(&writer, 3U, 61U);
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    }
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    CHECK(emit_fixture(directory, "spatial", &payload, &decoded) ==
          EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/*
 * Exercise the 2x2 spatial vectors directly: a 16x16 gradient whose third block
 * row is splits, each split's TL child a 2x2 spatial copy at a different index
 * (2,3,4,5 -- the entries Docs/Stream scrambled). The per-pixel gradient makes a
 * wrong table entry copy different pixels, so the Decomp7 cross-check catches it.
 */
static int make_spatial2x2(const char *directory)
{
    enum { W = 16U, H = 16U };
    MbPixel source_pixels[W * H];
    MbPixel decoded_pixels[W * H];
    MbFrame source = { W, H, W, source_pixels };
    MbFrame decoded = { W, H, W, decoded_pixels };
    /* Block x-origin -> 2x2 spatial index for that split's TL child. */
    static const unsigned splits[4][2] = {
        { 0U, 2U }, { 4U, 3U }, { 8U, 4U }, { 12U, 5U }
    };
    ReplayBuffer payload;
    unsigned i;

    for (i = 0U; i < W * H; ++i) {
        unsigned x = i % W;
        unsigned y = i / W;
        source_pixels[i] = (MbPixel){ (uint8_t)((x * 5U + y * 3U) & 31U),
                                      (uint8_t)((x + 2U) & 31U),
                                      (uint8_t)((y + 9U) & 31U) };
    }
    replay_buffer_init(&payload);
    {
        ReplayBitWriter writer;
        unsigned bx;
        unsigned by;

        replay_bitwriter_init(&writer, &payload);
        for (by = 0U; by < H; by += 4U) {
            for (bx = 0U; bx < W; bx += 4U) {
                unsigned split = 4U;
                unsigned s;

                if (by == 8U) {
                    for (s = 0U; s < 4U; ++s) {
                        if (splits[s][0] == bx) {
                            split = s;
                        }
                    }
                }
                if (split == 4U) {
                    put_data4x4(&writer, &source, bx, by);
                    continue;
                }
                /* `01` split; TL is a 2x2 spatial move, the rest data. */
                replay_bitwriter_write(&writer, 0U, 1U);
                replay_bitwriter_write(&writer, 1U, 1U);
                replay_bitwriter_write(&writer, 0U, 1U); /* TL `0` move */
                put_move_code(&writer, 3U, 56U + splits[split][1]);
                put_data2x2(&writer, &source, bx + 2U, by);
                put_data2x2(&writer, &source, bx, by + 2U);
                put_data2x2(&writer, &source, bx + 2U, by + 2U);
            }
        }
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    }
    CHECK(codec_movingblocks_verify_frame(payload.data, payload.size, NULL,
                                          &decoded, NULL, NULL) == REPLAY_OK);
    CHECK(emit_fixture(directory, "spatial2x2", &payload, &decoded) ==
          EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/*
 * End-to-end: encode a synthetic key frame and a following inter frame with the
 * real type 7 encoder, then hand both payloads to the compiled Decomp7. The
 * encoder mixes data, split, spatial, stationary and temporal blocks, so a
 * match confirms the whole encoder output decodes on Acorn code exactly as the
 * encoder reconstructed it.
 */
static int make_encoded(const char *directory)
{
    enum { W = 16U, H = 16U };
    MbPixel source_pixels[W * H];
    MbPixel key_recon[W * H];
    MbPixel inter_recon[W * H];
    MbFrame source = { W, H, W, source_pixels };
    MbFrame key = { W, H, W, key_recon };
    MbFrame inter = { W, H, W, inter_recon };
    MbFrame previous = { W, H, W, key_recon };
    CodecMovingBlocksEncodeOptions options;
    ReplayBuffer payload;
    unsigned i;

    for (i = 0U; i < W * H; ++i) {
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 3U) & 31U),
                                      (uint8_t)((i / W) & 31U),
                                      (uint8_t)((i % W) & 31U) };
    }
    options = (CodecMovingBlocksEncodeOptions){ 0, 0, 1, 1, 0U,
                                                MB_ENCODE_POLICY_LOWEST_ERROR,
                                                NULL };
    replay_buffer_init(&payload);
    CHECK(codec_movingblocks_encode_frame(&source, NULL, &options, &payload,
                                          &key, NULL) == REPLAY_OK);
    CHECK(emit_fixture(directory, "enc_key", &payload, &key) == EXIT_SUCCESS);

    /* Inter frame: most blocks copy the key reconstruction; a few change. */
    for (i = 0U; i < W * H; i += 7U) {
        source_pixels[i].y = (uint8_t)((source_pixels[i].y + 9U) & 31U);
    }
    options.allow_stationary = 1;
    options.allow_temporal = 1;
    CHECK(codec_movingblocks_encode_frame(&source, &previous, &options,
                                          &payload, &inter, NULL) == REPLAY_OK);
    CHECK(write_frame(directory, "enc_inter.previous.yuv555", &previous) ==
          EXIT_SUCCESS);
    CHECK(emit_fixture(directory, "enc_inter", &payload, &inter) ==
          EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(make_data(argv[1]) == EXIT_SUCCESS);
    CHECK(make_split(argv[1]) == EXIT_SUCCESS);
    CHECK(make_temporal(argv[1]) == EXIT_SUCCESS);
    CHECK(make_spatial(argv[1]) == EXIT_SUCCESS);
    CHECK(make_spatial2x2(argv[1]) == EXIT_SUCCESS);
    CHECK(make_encoded(argv[1]) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
