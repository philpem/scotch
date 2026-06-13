#ifndef REPLAY_MOVIE_H
#define REPLAY_MOVIE_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

/*
 * Shared movie-assembly helpers used by both the standalone joiner
 * (replay-join) and the direct-to-container encoder (replay-encode --output):
 * building the "helpful sprite" poster and turning raw PCM into a sound track
 * for replay_ae7_write. Keeping these in the library avoids duplicating the
 * exact byte layout in two tools.
 */

/*
 * Wrap raw 16bpp (bgr555, R in the low bits) pixels into a complete RISC OS
 * spritefile (the poster !ARPlayer displays). `out` is cleared first. Rows are
 * padded to a word; the sprite is 16bpp with square pixels at the movie's true
 * dimensions.
 */
ReplayStatus replay_build_poster(const uint8_t *pixels, unsigned width,
                                 unsigned height, ReplayBuffer *out);

/*
 * Encode raw signed-16 little-endian interleaved PCM (as produced by
 * `ffmpeg -f s16le`) into a sound track ready for replay_ae7_write. `encode`
 * selects the sub-format:
 *   "vidc-e8" / "vidc-log"  format 1, 8-bit VIDC exponential (default)
 *   "signed-8"              format 1, 8-bit signed linear
 *   "signed-16"             format 1, 16-bit signed linear
 *   "adpcm" / "adpcm2"      format 2 "adpcm", IMA ADPCM (encoded per chunk)
 *   "adpcm-sounda4"         format 1 built-in SoundA4 (4-bit ADPCM)
 * The encoded bytes (for the linear formats) are appended to `encoded`; for the
 * ADPCM paths `encoded` receives a copy of the raw PCM that the writer encodes
 * per chunk. `track` is filled to reference whichever buffer holds the data, so
 * `encoded` must outlive `track`'s use. On bad input a message is written to
 * `error` (when non-NULL) and REPLAY_INVALID_ARGUMENT is returned.
 */
ReplayStatus replay_build_pcm_track(const uint8_t *pcm, size_t pcm_size,
                                    const char *encode, unsigned rate_hz,
                                    unsigned channels, ReplayBuffer *encoded,
                                    ReplayAe7WriteTrack *track, char *error,
                                    size_t error_size);

#endif /* REPLAY_MOVIE_H */
