/* Helper for test_transcode.sh: confirm a frame in replay-transcode's raw RGB24
 * stream equals the corpus 6Y5UV frame converted with the same preview path.
 *
 * usage: transcode_frame_check RGB_STREAM WIDTH HEIGHT FRAME_INDEX CORPUS_6Y5UV
 */

#include "replay/mb_color.h"
#include "replay/mb_frame.h"
#include "replay/replay_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long s;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (s = ftell(f)) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    rewind(f);
    b = malloc((size_t)s ? (size_t)s : 1);
    if (b == NULL || (s > 0 && fread(b, 1, (size_t)s, f) != (size_t)s)) {
        fprintf(stderr, "read error %s\n", path);
        exit(2);
    }
    fclose(f);
    *len = (size_t)s;
    return b;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s RGB WIDTH HEIGHT FRAME CORPUS_6Y5UV\n",
                argv[0]);
        return 2;
    }
    unsigned width = (unsigned)atoi(argv[2]);
    unsigned height = (unsigned)atoi(argv[3]);
    unsigned frame = (unsigned)atoi(argv[4]);
    size_t pixels = (size_t)width * height;

    size_t rgb_len, yuv_len;
    uint8_t *rgb_stream = read_file(argv[1], &rgb_len);
    uint8_t *yuv = read_file(argv[5], &yuv_len);

    if (yuv_len != pixels * 3) {
        fprintf(stderr, "corpus frame is %zu bytes; expected %zu\n",
                yuv_len, pixels * 3);
        return 2;
    }
    if ((size_t)(frame + 1) * pixels * 3 > rgb_len) {
        fprintf(stderr, "frame %u beyond stream\n", frame);
        return 2;
    }

    uint8_t *rgb_ref = malloc(pixels * 3);
    if (rgb_ref == NULL)
        return 2;

    MbFrame f;
    f.width = width;
    f.height = height;
    f.stride = width;
    f.pixels = (MbPixel *)yuv;
    if (mb_color_6y5uv_to_rgb24(&f, rgb_ref, (size_t)width * 3) != REPLAY_OK)
        return 2;

    int diff = memcmp(rgb_ref, rgb_stream + (size_t)frame * pixels * 3,
                      pixels * 3);
    printf("frame %u: %s\n", frame, diff == 0 ? "MATCH" : "MISMATCH");

    free(rgb_ref);
    free(yuv);
    free(rgb_stream);
    return diff == 0 ? 0 : 1;
}
