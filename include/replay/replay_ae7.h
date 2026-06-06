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
    unsigned width;
    unsigned height;
    unsigned pixel_depth;
    double frames_per_second;
    unsigned sound_codec;
    unsigned sound_rate;
    unsigned sound_channels;
    unsigned sound_precision;
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
