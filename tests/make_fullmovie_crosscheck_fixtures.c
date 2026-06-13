#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_movingblocks.h"
#include "replay/codec_movingblocksbeta.h"
#include "replay/codec_movingblockshq.h"
#include "replay/codec_supermovingblocks.h"

/*
 * Standing full-movie cross-check gate. Encode a multi-frame synthetic scene
 * with the real encoder for a chosen codec (7, 17 or 19), writing each frame's
 * payload and the exact working-space frame the encoder reconstructed. The
 * companion shell test decodes every payload on the genuine compiled Decomp
 * module and compares byte-for-byte, frame by frame.
 *
 * Unlike the focused fixtures (which exercise one block mode each), the scene
 * mixes a panning texture (temporal copies at varied vectors), a 4- and
 * 2-periodic tiled band (4x4 and 2x2 spatial copies, plus splits), a static
 * band (stationary copies) and a per-frame noise patch (data). This is the
 * coverage that would have caught the type 7 2x2-spatial table bug, where the
 * encoder and verifier agreed with each other but disagreed with real Acorn
 * code on a real, spatial-heavy frame.
 *
 * Source pixels are generated directly in each codec's working precision, so a
 * mismatch points at the bitstream rather than colour quantisation.
 */

#define MOVIE_W 80U
#define MOVIE_H 48U
#define MOVIE_FRAMES 8U

static int write_bytes(const char *directory, const char *name,
                       const void *data, size_t size)
{
    char path[1024];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) <= 0) {
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

/* Write a frame as the harness reads it: one Y, U, V byte per pixel. */
static int write_frame(const char *directory, const char *name,
                       const MbFrame *frame)
{
    uint8_t packed[MOVIE_W * MOVIE_H * 3U];
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
    return write_bytes(directory, name, packed, offset);
}

/* A small, deterministic per-frame pan that visits several motion vectors. */
static int pan_x(unsigned f) { static const int p[8] = { 0, 1, -1, 2, -2, 3, 1, -3 }; return p[f & 7U]; }
static int pan_y(unsigned f) { static const int p[8] = { 0, -1, 1, -2, 2, -1, -3, 1 }; return p[f & 7U]; }

/* Rich 2D texture with both 4- and 2-periodic structure, in [0, ymax]. */
static unsigned texture(int x, int y, unsigned ymax)
{
    unsigned ax = (unsigned)(x < 0 ? -x : x);
    unsigned ay = (unsigned)(y < 0 ? -y : y);
    unsigned tile = (ax & 3U) * 5U + (ay & 1U) * 11U; /* 4x / 2y periodic */
    unsigned grad = (ax + ay) * 2U;

    return (tile + grad) % (ymax + 1U);
}

static void gen_frame(MbPixel *pixels, unsigned f, unsigned ymax)
{
    unsigned y;

    for (y = 0U; y < MOVIE_H; ++y) {
        unsigned x;
        for (x = 0U; x < MOVIE_W; ++x) {
            MbPixel *p = &pixels[(size_t)y * MOVIE_W + x];
            /* Per-4x4-block coordinates used by the crafted spatial bands. */
            unsigned blk = ((x >> 2) * 3U + (y >> 2) * 5U + f * 4U);
            unsigned xx = x & 1U;             /* column parity within a 2x2 */
            unsigned yq = (y & 3U) >> 1U;     /* top (0) vs bottom (1) 2x2 */
            unsigned xh = (x & 3U) >> 1U;     /* left (0) vs right (1) 2x2 */
            unsigned yy;
            unsigned u = (texture((int)x + 3, (int)y, 31U)) & 31U;
            unsigned v = (texture((int)x, (int)y + 5, 31U)) & 31U;

            if (x < 16U) {
                /* Static band -> stationary copies on inter frames. */
                yy = texture((int)x, (int)y, ymax);
            } else if (x < 32U) {
                /* Panning texture -> temporal copies at varied vectors. */
                yy = texture((int)x - pan_x(f), (int)y - pan_y(f), ymax);
            } else if (x < 48U) {
                /*
                 * Left 2x2 == right 2x2 (luma independent of which half), top
                 * != bottom: forces a split whose right children copy the left
                 * via the 2x2 spatial (-2,0) vector. Uniform chroma so the copy
                 * matches on every component.
                 */
                yy = (xx * 9U + yq * 17U + blk) % (ymax + 1U);
                u = 16U;
                v = 16U;
            } else if (x < 64U) {
                /*
                 * Top 2x2 == bottom 2x2, left != right: forces a split whose
                 * lower children copy the upper via the 2x2 spatial (0,-2)
                 * vector -- one of the indices the type 7 table scrambled.
                 */
                yy = (xx * 9U + xh * 17U + blk) % (ymax + 1U);
                u = 16U;
                v = 16U;
            } else {
                /*
                 * Luma independent of the 4x4 block column (every block in a
                 * band row is identical), varying per row and per frame: a 4x4
                 * block copies its left neighbour via the (-4,0) spatial vector.
                 */
                yy = ((x & 3U) * 7U + (y & 3U) * 11U + (y >> 2) * 2U + f * 3U) %
                     (ymax + 1U);
                u = 16U;
                v = 16U;
            }
            p->y = (uint8_t)yy;
            p->u = (uint8_t)u;
            p->v = (uint8_t)v;
        }
    }
}

/* Aggregate block-mode tally, for verifying the scene's coverage. */
typedef struct {
    size_t data4x4, temporal4x4, spatial4x4, split4x4;
    size_t data2x2, stationary2x2, temporal2x2, spatial2x2, stationary4x4;
} BlockTally;

static ReplayStatus encode_frame(unsigned codec, const MbFrame *source,
                                 const MbFrame *previous, ReplayBuffer *output,
                                 MbFrame *reconstructed, BlockTally *tally)
{
    int inter = previous != NULL;
    int allow_copy_prev = inter; /* stationary/temporal need a previous frame */
    ReplayStatus status;

    if (codec == 7U) {
        CodecMovingBlocksEncodeOptions o = {
            allow_copy_prev, allow_copy_prev, 1, 1, 4U,
            MB_ENCODE_POLICY_LOWEST_ERROR, NULL
        };
        CodecMovingBlocksEncodeStats s = { 0 };
        status = codec_movingblocks_encode_frame(source, previous, &o, output,
                                                 reconstructed, &s);
        tally->spatial4x4 += s.spatial4x4_blocks;
        tally->spatial2x2 += s.spatial2x2_blocks;
        tally->temporal4x4 += s.temporal4x4_blocks;
        tally->temporal2x2 += s.temporal2x2_blocks;
        tally->stationary4x4 += s.stationary4x4_blocks;
        tally->split4x4 += s.split4x4_blocks;
        tally->data4x4 += s.data4x4_blocks;
        return status;
    }
    if (codec == 17U) {
        CodecMovingBlocksHqEncodeOptions o = {
            allow_copy_prev, allow_copy_prev, 1, 1, 4U,
            MB_ENCODE_POLICY_LOWEST_ERROR, NULL
        };
        CodecMovingBlocksHqEncodeStats s = { 0 };
        status = codec_movingblockshq_encode_frame(source, previous, &o, output,
                                                   reconstructed, &s);
        tally->spatial4x4 += s.spatial4x4_blocks;
        tally->spatial2x2 += s.spatial2x2_blocks;
        tally->temporal4x4 += s.temporal4x4_blocks;
        tally->temporal2x2 += s.temporal2x2_blocks;
        tally->split4x4 += s.split4x4_blocks;
        tally->data4x4 += s.data4x4_blocks;
        return status;
    }
    if (codec == 20U) {
        CodecMovingBlocksBetaEncodeOptions o = {
            allow_copy_prev, allow_copy_prev, 1, 1, 4U,
            MB_ENCODE_POLICY_LOWEST_ERROR, NULL
        };
        CodecMovingBlocksBetaEncodeStats s = { 0 };
        status = codec_movingblocksbeta_encode_frame(source, previous, &o,
                                                     output, reconstructed, &s);
        tally->spatial4x4 += s.spatial4x4_blocks;
        tally->spatial2x2 += s.spatial2x2_blocks;
        tally->temporal4x4 += s.temporal4x4_blocks;
        tally->temporal2x2 += s.temporal2x2_blocks;
        tally->split4x4 += s.split4x4_blocks;
        tally->data4x4 += s.data4x4_blocks;
        return status;
    }
    {
        CodecSuperMovingBlocksEncodeOptions o = {
            allow_copy_prev, allow_copy_prev, 1, 1, 4U,
            CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR, NULL
        };
        CodecSuperMovingBlocksEncodeStats s = { 0 };
        status = codec_supermovingblocks_encode_frame(source, previous, &o,
                                                       output, reconstructed,
                                                       &s);
        tally->spatial4x4 += s.spatial4x4_blocks;
        tally->spatial2x2 += s.spatial2x2_blocks;
        tally->temporal4x4 += s.temporal4x4_blocks;
        tally->temporal2x2 += s.temporal2x2_blocks;
        tally->split4x4 += s.split4x4_blocks;
        tally->data4x4 += s.data4x4_blocks;
        return status;
    }
}

int main(int argc, char **argv)
{
    static MbPixel source_pixels[MOVIE_W * MOVIE_H];
    static MbPixel recon_pixels[MOVIE_W * MOVIE_H];
    static MbPixel previous_pixels[MOVIE_W * MOVIE_H];
    MbFrame source = { MOVIE_W, MOVIE_H, MOVIE_W, source_pixels };
    MbFrame reconstructed = { MOVIE_W, MOVIE_H, MOVIE_W, recon_pixels };
    MbFrame previous = { MOVIE_W, MOVIE_H, MOVIE_W, previous_pixels };
    ReplayBuffer payload;
    unsigned codec;
    unsigned ymax;
    unsigned f;

    CHECK(argc == 3);
    codec = (unsigned)strtoul(argv[2], NULL, 10);
    CHECK(codec == 7U || codec == 17U || codec == 19U || codec == 20U);
    ymax = (codec == 19U || codec == 20U) ? 63U : 31U;

    replay_buffer_init(&payload);
    {
        BlockTally tally = { 0 };

        for (f = 0U; f < MOVIE_FRAMES; ++f) {
            char name[64];
            const MbFrame *previous_arg = f == 0U ? NULL : &previous;

            gen_frame(source_pixels, f, ymax);
            CHECK(encode_frame(codec, &source, previous_arg, &payload,
                               &reconstructed, &tally) == REPLAY_OK);
            CHECK(snprintf(name, sizeof(name), "f%03u.payload", f) > 0);
            CHECK(write_bytes(argv[1], name, payload.data, payload.size) ==
                  EXIT_SUCCESS);
            CHECK(snprintf(name, sizeof(name), "f%03u.recon", f) > 0);
            CHECK(write_frame(argv[1], name, &reconstructed) == EXIT_SUCCESS);
            memcpy(previous_pixels, recon_pixels, sizeof(previous_pixels));
        }
        /* Report coverage so a regression that stops exercising a mode (and so
           silently weakens the gate) is visible in the test log. */
        fprintf(stderr,
                "codec %u coverage: data4x4=%zu temporal4x4=%zu spatial4x4=%zu "
                "split4x4=%zu stationary4x4=%zu temporal2x2=%zu spatial2x2=%zu\n",
                codec, tally.data4x4, tally.temporal4x4, tally.spatial4x4,
                tally.split4x4, tally.stationary4x4, tally.temporal2x2,
                tally.spatial2x2);
        /* The gate is only meaningful if the scene actually exercises the copy
           families; fail loudly if a future change stops doing so. */
        CHECK(tally.spatial4x4 > 0U);
        CHECK(tally.spatial2x2 > 0U);
        CHECK(tally.temporal4x4 > 0U);
        CHECK(tally.split4x4 > 0U);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}
