/* Build a synthetic type-23 (6Y6Y5U5V 4:2:2) movie plus the expected RGB24
 * stream, for test_transcode_type23.sh.
 *
 * Chroma is held constant within each horizontal pixel pair so the type-23
 * pack/unpack round-trip is lossless; the expected RGB is therefore computed
 * directly from the source frames (independent of the transcoder's decode path),
 * making the comparison a real end-to-end check of the container walk + unpack.
 *
 * usage: make_type23_movie OUT_MOVIE OUT_EXPECTED_RGB W H FRAMES FRAMES_PER_CHUNK
 */

#include "replay/mb_color.h"
#include "replay/mb_frame.h"
#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_status.h"
#include "replay/replay_type23.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *msg)
{
    fprintf(stderr, "make_type23_movie: %s\n", msg);
    exit(1);
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL || (len > 0 && fwrite(data, 1, len, f) != len))
        fail("write error");
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 7)
        fail("usage: OUT_MOVIE OUT_EXPECTED W H FRAMES FPC");

    const char *movie_path = argv[1];
    const char *expected_path = argv[2];
    unsigned width = (unsigned)atoi(argv[3]);
    unsigned height = (unsigned)atoi(argv[4]);
    unsigned frames = (unsigned)atoi(argv[5]);
    unsigned fpc = (unsigned)atoi(argv[6]);
    size_t pixel_count = (size_t)width * height;

    if (width == 0 || (width & 1u) || height == 0 || frames == 0)
        fail("bad geometry");

    size_t frame_bytes = 0;
    if (replay_type23_frame_bytes(width, height, &frame_bytes) != REPLAY_OK)
        fail("frame_bytes");

    uint8_t **blobs = calloc(frames, sizeof *blobs);
    const uint8_t **frame_data = calloc(frames, sizeof *frame_data);
    size_t *frame_size = calloc(frames, sizeof *frame_size);
    MbPixel *pix = malloc(pixel_count * sizeof *pix);
    uint8_t *rgb = malloc(pixel_count * 3);
    if (!blobs || !frame_data || !frame_size || !pix || !rgb)
        fail("oom");

    ReplayBuffer expected;
    replay_buffer_init(&expected);

    for (unsigned f = 0; f < frames; f++) {
        for (unsigned y = 0; y < height; y++) {
            for (unsigned x = 0; x < width; x++) {
                unsigned pair = x / 2;
                MbPixel *p = &pix[(size_t)y * width + x];
                p->y = (uint8_t)((x + y + f * 7u) & 0x3Fu);     /* 6-bit luma */
                /* chroma depends only on (pair, y, f): identical across a pair */
                p->u = (uint8_t)((pair * 3u + f) & 0x1Fu);
                p->v = (uint8_t)((y * 2u + f) & 0x1Fu);
            }
        }
        MbFrame frame;
        frame.width = width;
        frame.height = height;
        frame.stride = width;
        frame.pixels = pix;

        blobs[f] = malloc(frame_bytes);
        if (blobs[f] == NULL)
            fail("oom");
        if (replay_type23_pack_frame(&frame, blobs[f], frame_bytes) != REPLAY_OK)
            fail("pack");
        frame_data[f] = blobs[f];
        frame_size[f] = frame_bytes;

        if (mb_color_6y5uv_to_rgb24(&frame, rgb, (size_t)width * 3) != REPLAY_OK)
            fail("color");
        if (replay_buffer_append(&expected, rgb, pixel_count * 3) != REPLAY_OK)
            fail("oom");
    }

    ReplayAe7WriteOptions opt;
    memset(&opt, 0, sizeof opt);
    opt.title = "type23 test";
    opt.video_codec = 23;
    opt.width = width;
    opt.height = height;
    opt.pixel_depth = 16;
    opt.frames_per_second = 12.5;
    opt.frame_data = frame_data;
    opt.frame_size = frame_size;
    opt.frame_count = frames;
    opt.frames_per_chunk = fpc;

    ReplayBuffer movie;
    replay_buffer_init(&movie);
    char err[256];
    if (replay_ae7_write(&opt, &movie, err, sizeof err) != REPLAY_OK)
        fail(err);

    write_file(movie_path, movie.data, movie.size);
    write_file(expected_path, expected.data, expected.size);

    fprintf(stderr, "make_type23_movie: %u frames %ux%u, fpc=%u -> %s (%zu B)\n",
            frames, width, height, fpc, movie_path, movie.size);

    replay_buffer_free(&movie);
    replay_buffer_free(&expected);
    for (unsigned f = 0; f < frames; f++)
        free(blobs[f]);
    free(blobs); free(frame_data); free(frame_size); free(pix); free(rgb);
    return 0;
}
