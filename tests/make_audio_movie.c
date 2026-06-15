/* Build a synthetic movie with a signed-16 stereo sound track (and trivial
 * type-23 video, so no ARM module is needed) plus the expected interleaved
 * signed-16 LE PCM, for test_transcode_audio.sh.
 *
 * signed-16 decode is the identity, so the expected PCM equals the input track:
 * this checks the transcoder's container sound walk, format detection, stereo
 * interleaving and WAV framing exactly.
 *
 * usage: make_audio_movie OUT_MOVIE OUT_EXPECTED_PCM
 */

#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_status.h"
#include "replay/replay_type23.h"
#include "replay/mb_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 8x2 type-23 frames are 22 bytes each, so the chunk video region stays even
 * (the writer pads an odd video region, which would break type-23's exact
 * frame-size multiple). */
#define WIDTH 8u
#define HEIGHT 2u
#define FRAMES 25u
#define RATE 8000u
#define STEREO_FRAMES (RATE) /* 1.0s at 25fps for 25 frames */

static void fail(const char *m) { fprintf(stderr, "make_audio_movie: %s\n", m); exit(1); }

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL || (len > 0 && fwrite(data, 1, len, f) != len))
        fail("write error");
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        fail("usage: OUT_MOVIE OUT_EXPECTED_PCM");

    /* Sound track: interleaved L,R signed-16 LE. */
    ReplayBuffer pcm;
    replay_buffer_init(&pcm);
    for (unsigned k = 0; k < STEREO_FRAMES; k++) {
        int l = (int)((k * 3u) % 65536u) - 32768;
        int r = (int)((k * 7u) % 65536u) - 32768;
        uint8_t b[4];
        b[0] = (uint8_t)l; b[1] = (uint8_t)((unsigned)l >> 8);
        b[2] = (uint8_t)r; b[3] = (uint8_t)((unsigned)r >> 8);
        if (replay_buffer_append(&pcm, b, 4) != REPLAY_OK)
            fail("oom");
    }

    /* Trivial type-23 video frames (all zero). */
    size_t frame_bytes = 0;
    if (replay_type23_frame_bytes(WIDTH, HEIGHT, &frame_bytes) != REPLAY_OK)
        fail("frame_bytes");
    uint8_t *blob = calloc(1, frame_bytes ? frame_bytes : 1);
    MbPixel pix[WIDTH * HEIGHT];
    MbFrame frame;
    frame.width = WIDTH; frame.height = HEIGHT; frame.stride = WIDTH;
    frame.pixels = pix;
    memset(pix, 0, sizeof pix);
    if (!blob || replay_type23_pack_frame(&frame, blob, frame_bytes) != REPLAY_OK)
        fail("pack");

    const uint8_t *frame_data[FRAMES];
    size_t frame_size[FRAMES];
    for (unsigned f = 0; f < FRAMES; f++) {
        frame_data[f] = blob;
        frame_size[f] = frame_bytes;
    }

    ReplayAe7WriteTrack track;
    memset(&track, 0, sizeof track);
    track.codec = REPLAY_AE7_SOUND_VIDC_LOG; /* format 1 */
    track.rate_hz = RATE;
    track.channels = 2;
    track.precision_bits = 16;
    track.label = "16 bits per sample (signed)";
    track.data = pcm.data;
    track.size = pcm.size;

    ReplayAe7WriteOptions opt;
    memset(&opt, 0, sizeof opt);
    opt.title = "audio test";
    opt.video_codec = 23;
    opt.width = WIDTH;
    opt.height = HEIGHT;
    opt.pixel_depth = 16;
    opt.frames_per_second = 25.0;
    opt.frame_data = frame_data;
    opt.frame_size = frame_size;
    opt.frame_count = FRAMES;
    opt.frames_per_chunk = FRAMES;
    opt.tracks = &track;
    opt.track_count = 1;

    ReplayBuffer movie;
    replay_buffer_init(&movie);
    char err[256];
    if (replay_ae7_write(&opt, &movie, err, sizeof err) != REPLAY_OK)
        fail(err);

    write_file(argv[1], movie.data, movie.size);
    write_file(argv[2], pcm.data, pcm.size);
    fprintf(stderr, "make_audio_movie: %u stereo frames @ %u Hz -> %s\n",
            STEREO_FRAMES, RATE, argv[1]);

    replay_buffer_free(&movie);
    replay_buffer_free(&pcm);
    free(blob);
    return 0;
}
