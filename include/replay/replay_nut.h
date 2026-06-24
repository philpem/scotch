#ifndef REPLAY_NUT_H
#define REPLAY_NUT_H

/* A small, dependency-free muxer for the NUT container format (the low-overhead
 * container designed by the FFmpeg/MPlayer developers). It writes a stream that
 * ffmpeg can demux directly from a pipe, so a tool can emit interleaved audio
 * and video without a separate sidecar file or a manually specified geometry.
 *
 * The muxer is deliberately codec-agnostic: each stream carries a NUT codec tag
 * (fourcc) and optional extradata, so besides the raw rawvideo/PCM streams the
 * transcoder uses, a future caller can mux already-compressed packets (e.g. a
 * MovieFS re-encapsulation) and let ffmpeg decode them.
 *
 * Frames are coded with NUT's explicit "FLAG_CODED" framing (one frame-code
 * table row covers all 256 codes), every frame carries an absolute pts and a
 * header checksum, and a syncpoint is emitted whenever the distance since the
 * last one reaches max_distance. Output is purely sequential (no seeking), so
 * no index is written -- ideal for a pipe.
 *
 * The bitstream layout mirrors ffmpeg's own nutenc.c field-for-field; the
 * ffmpeg-gated round-trip test (tests/test_transcode_nut.sh) is the end-to-end
 * correctness oracle.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "replay/replay_status.h"

typedef enum {
    REPLAY_NUT_VIDEO = 0,
    REPLAY_NUT_AUDIO = 1
} ReplayNutStreamClass;

typedef struct {
    ReplayNutStreamClass cls;
    /* NUT codec tag, in the on-disk byte order ffmpeg expects (it writes the
     * tag little-endian). For rawvideo RGB24 this is {'R','G','B',24}; for
     * pcm_s16le it is {'P','S','D',16}. */
    uint8_t fourcc[4];
    /* Presentation time of a frame with pts P is P * tb_num / tb_den seconds. */
    unsigned tb_num;
    unsigned tb_den;
    /* Video geometry (cls == REPLAY_NUT_VIDEO). */
    unsigned width;
    unsigned height;
    /* Audio parameters (cls == REPLAY_NUT_AUDIO). */
    unsigned sample_rate;
    unsigned channels;
    /* Optional codec-specific data; NULL/0 for rawvideo and PCM. */
    const uint8_t *extradata;
    size_t extradata_len;
} ReplayNutStream;

typedef struct ReplayNutMuxer ReplayNutMuxer;

/* Open a muxer writing to `out` and emit the file header, main header and all
 * stream headers. The `streams` array is copied. Returns NULL on error with a
 * message in `err`. */
ReplayNutMuxer *replay_nut_open(FILE *out, const ReplayNutStream *streams,
                                size_t stream_count, char *err, size_t errlen);

/* Append one coded frame for stream `stream_index` at presentation time `pts`
 * (in that stream's time base). `keyframe` marks a key/sync frame. A syncpoint
 * is emitted first when required. */
ReplayStatus replay_nut_write_frame(ReplayNutMuxer *m, size_t stream_index,
                                    int64_t pts, int keyframe,
                                    const uint8_t *data, size_t len);

/* Flush and release the muxer. Does not close `out`. */
ReplayStatus replay_nut_close(ReplayNutMuxer *m);

#endif
