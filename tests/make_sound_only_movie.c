/* Build a synthetic sound-only movie (video format 0, no frames) carrying a
 * signed-16 stereo sound track, plus the expected interleaved signed-16 LE PCM,
 * for test_transcode_sound_only.sh.
 *
 * Video format 0 means "no video track": there is no decompressor for it, so
 * replay-transcode must produce the audio and not try to load a Decomp0 module
 * even when --modules-dir is supplied. signed-16 decode is the identity, so the
 * expected PCM equals the input track.
 *
 * usage: make_sound_only_movie OUT_MOVIE OUT_EXPECTED_PCM
 */

#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 8000u
#define STEREO_FRAMES (RATE * 2u) /* 2.0s of audio */

static void fail(const char *m) { fprintf(stderr, "make_sound_only_movie: %s\n", m); exit(1); }

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

    ReplayAe7WriteTrack track;
    memset(&track, 0, sizeof track);
    track.codec = REPLAY_AE7_SOUND_VIDC_LOG; /* format 1 */
    track.rate_hz = RATE;
    track.channels = 2;
    track.precision_bits = 16;
    track.label = "16 bits per sample (signed)";
    track.data = pcm.data;
    track.size = pcm.size;

    /* Sound-only: no frames, video format 0. The writer forces 0x0 dimensions. */
    ReplayAe7WriteOptions opt;
    memset(&opt, 0, sizeof opt);
    opt.title = "sound-only test";
    opt.video_codec = 0;
    opt.frames_per_second = 25.0;
    opt.frames_per_chunk = 25;
    opt.tracks = &track;
    opt.track_count = 1;

    ReplayBuffer movie;
    replay_buffer_init(&movie);
    char err[256];
    if (replay_ae7_write(&opt, &movie, err, sizeof err) != REPLAY_OK)
        fail(err);

    write_file(argv[1], movie.data, movie.size);
    write_file(argv[2], pcm.data, pcm.size);
    fprintf(stderr, "make_sound_only_movie: %u stereo frames @ %u Hz -> %s\n",
            STEREO_FRAMES, RATE, argv[1]);

    replay_buffer_free(&movie);
    replay_buffer_free(&pcm);
    return 0;
}
