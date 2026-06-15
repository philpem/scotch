#ifndef REPLAY_AE7_H
#define REPLAY_AE7_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_status.h"

#define REPLAY_AE7_TEXT_MAX 128U

typedef struct {
    uint64_t file_offset;
    uint64_t video_bytes;
    uint64_t sound_bytes;
    unsigned sound_tracks;
} ReplayAe7Chunk;

typedef struct {
    char title[REPLAY_AE7_TEXT_MAX];
    char copyright[REPLAY_AE7_TEXT_MAX];
    char author[REPLAY_AE7_TEXT_MAX];
    unsigned video_codec;
    /* Trailing text after the video-format number, e.g. the decompressor name
     * for format 15 ("15 video1"). Empty when none. */
    char video_label[REPLAY_AE7_TEXT_MAX];
    unsigned width;
    unsigned height;
    unsigned pixel_depth;
    /* Trailing text after the bits-per-pixel number, carrying the colour model
     * ("16 bits per pixel [6Y5UV]", "[YUV]", "[YIQ]") and, for 8bpp, the
     * "palette <offset>" pointer. Empty when none. */
    char pixel_label[REPLAY_AE7_TEXT_MAX];
    double frames_per_second;
    unsigned sound_codec;
    /* Trailing text after the sound-format number: the decompressor name for
     * format 2 ("2 adpcm", "2 GSM"). Empty when none. */
    char sound_format_label[REPLAY_AE7_TEXT_MAX];
    unsigned sound_rate;
    unsigned sound_channels;
    /* Trailing text after the channel count, e.g. "REVER" for reversed stereo. */
    char sound_channels_label[REPLAY_AE7_TEXT_MAX];
    unsigned sound_precision;
    /* Trailing text after the bits-per-sample number, selecting the format-1
     * sound decoder per ToUseSound: "ADPCM", "LIN", "UNSIGN", or a VIDC/µ-law
     * note (otherwise exponential). Empty when none. */
    char sound_precision_label[REPLAY_AE7_TEXT_MAX];
    unsigned frames_per_chunk;
    unsigned last_chunk;
    uint64_t even_chunk_bytes;
    uint64_t odd_chunk_bytes;
    uint64_t catalogue_offset;
    uint64_t sprite_offset;
    uint64_t sprite_bytes;
    int64_t key_frame_offset;
    ReplayAe7Chunk *chunks;
    size_t chunk_count;
} ReplayAe7Movie;

/*
 * Parse the textual AE7 header and chunk catalogue from a complete movie.
 * Header field 15 is the last zero-based chunk number, so the catalogue has
 * last_chunk + 1 entries. replay_ae7_movie_destroy releases that catalogue.
 */
ReplayStatus replay_ae7_parse(const uint8_t *data, size_t size,
                              ReplayAe7Movie *movie,
                              char *error, size_t error_size);
void replay_ae7_movie_destroy(ReplayAe7Movie *movie);

#endif
