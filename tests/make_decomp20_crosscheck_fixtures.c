#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "replay/codec_movingblocksbeta.h"
#include "replay/replay_buffer.h"

/*
 * Encode a synthetic 6Y6UV key frame and a following inter frame with the real
 * type 20 encoder, then hand both payloads to the compiled Decomp20. The encoder
 * mixes data (delta-coded chroma), split, spatial, stationary and temporal
 * blocks, so a byte-exact match confirms the whole type 20 output -- including
 * the chroma-delta data format -- decodes on real Acorn code as encoded.
 */
#define W 16U
#define H 16U

static int write_bytes(const char *dir, const char *name, const void *data,
                       size_t size)
{
    char path[1024];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) <= 0) {
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

/* Write a frame as the harness reads 6y6uv: one Y, U, V byte per pixel. */
static int write_frame(const char *dir, const char *name, const MbFrame *frame)
{
    uint8_t packed[W * H * 3U];
    size_t offset = 0U;
    unsigned y;

    for (y = 0U; y < frame->height; ++y) {
        unsigned x;
        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *p = &frame->pixels[(size_t)y * frame->stride + x];
            packed[offset++] = p->y;
            packed[offset++] = p->u;
            packed[offset++] = p->v;
        }
    }
    return write_bytes(dir, name, packed, offset);
}

static int emit(const char *dir, const char *stem, const ReplayBuffer *payload,
                const MbFrame *expected)
{
    char name[128];

    CHECK(snprintf(name, sizeof(name), "%s.mb20", stem) > 0);
    CHECK(write_bytes(dir, name, payload->data, payload->size) == EXIT_SUCCESS);
    CHECK(snprintf(name, sizeof(name), "%s.expected.6y6uv", stem) > 0);
    CHECK(write_frame(dir, name, expected) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    static MbPixel source_pixels[W * H];
    static MbPixel key_recon[W * H];
    static MbPixel inter_recon[W * H];
    MbFrame source = { W, H, W, source_pixels };
    MbFrame key = { W, H, W, key_recon };
    MbFrame inter = { W, H, W, inter_recon };
    MbFrame previous = { W, H, W, key_recon };
    CodecMovingBlocksBetaEncodeOptions options;
    ReplayBuffer payload;
    unsigned i;

    CodecMovingBlocksBetaVariant variant;

    CHECK(argc == 3);
    variant = strcmp(argv[2], "new") == 0 ? CODEC_MOVINGBLOCKSBETA_NEW
                                          : CODEC_MOVINGBLOCKSBETA_OLD;
    for (i = 0U; i < W * H; ++i) {
        unsigned x = i % W;
        unsigned y = i / W;
        source_pixels[i] = (MbPixel){ (uint8_t)((i * 3U) & 63U),
                                      (uint8_t)((x * 4U) & 63U),
                                      (uint8_t)((y * 4U) & 63U) };
    }
    options = (CodecMovingBlocksBetaEncodeOptions){
        0, 0, 1, 1, 0U, MB_ENCODE_POLICY_LOWEST_ERROR, NULL, variant
    };
    replay_buffer_init(&payload);
    CHECK(codec_movingblocksbeta_encode_frame(&source, NULL, &options, &payload,
                                              &key, NULL) == REPLAY_OK);
    CHECK(emit(argv[1], "enc_key", &payload, &key) == EXIT_SUCCESS);

    for (i = 0U; i < W * H; i += 7U) {
        source_pixels[i].y = (uint8_t)((source_pixels[i].y + 9U) & 63U);
    }
    options.allow_stationary = 1;
    options.allow_temporal = 1;
    CHECK(codec_movingblocksbeta_encode_frame(&source, &previous, &options,
                                              &payload, &inter,
                                              NULL) == REPLAY_OK);
    CHECK(write_frame(argv[1], "enc_inter.previous.6y6uv", &previous) ==
          EXIT_SUCCESS);
    CHECK(emit(argv[1], "enc_inter", &payload, &inter) == EXIT_SUCCESS);
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
