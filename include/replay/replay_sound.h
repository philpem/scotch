#ifndef REPLAY_SOUND_H
#define REPLAY_SOUND_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

/*
 * Encoders from signed 16-bit little-endian PCM (as produced by, e.g.,
 * `ffmpeg -f s16le`) to the Replay sound-format-1 sub-formats. The text label
 * in the ARMovie header's bits-per-sample field selects the player's decoder
 * (see ToUseSound): a label containing "LIN" picks signed/unsigned linear,
 * otherwise the 8-bit "exponential" VIDC companding is used.
 */
typedef enum {
    REPLAY_SOUND_VIDC_E8,  /* 8-bit VIDC exponential (SoundE8); nearest-match
                            * inversion of Acorn's ELogToLinTable */
    REPLAY_SOUND_SIGNED_8, /* 8-bit signed linear (SoundS8); high byte */
    REPLAY_SOUND_SIGNED_16 /* 16-bit signed linear (SoundS16); little-endian */
} ReplaySoundFormat;

/*
 * Append the encoded track to `out`. `samples` is interleaved (for stereo,
 * L,R,L,R...) signed 16-bit host-order PCM; `sample_count` counts individual
 * samples (frames * channels). E8 and signed-8 emit one byte per sample;
 * signed-16 emits two little-endian bytes per sample.
 */
ReplayStatus replay_sound_encode(ReplaySoundFormat format,
                                 const int16_t *samples, size_t sample_count,
                                 ReplayBuffer *out);

/* Header metadata for the chosen format: the bits-per-sample number and the
 * annotation text that follows it (which selects the player's decoder). */
unsigned replay_sound_format_bits(ReplaySoundFormat format);
const char *replay_sound_format_label(ReplaySoundFormat format);

/* Single-sample VIDC E8 codec, exposed for testing and reuse. */
uint8_t replay_sound_vidc_e8_from_s16(int16_t sample);
int16_t replay_sound_vidc_e8_to_s16(uint8_t code);

#endif
