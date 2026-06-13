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

/*
 * IMA/DVI ADPCM (Replay sound format "2 adpcm"), the canonical 7-Jul-92
 * reference codec Acorn used. The state persists across calls so a track can be
 * encoded chunk by chunk; the value of the state at a chunk's first sample is
 * written as that chunk's 4-byte header (valprev little-endian, then index, then
 * a pad byte) so the player can start decoding the chunk independently.
 */
typedef struct {
    int16_t predicted;  /* valprev: last reconstructed sample */
    int8_t step_index;  /* index into the step-size table, 0..88 */
} ReplaySoundAdpcmState;

/* Append `count` mono samples to `out` as 4-bit codes (two per byte, first
 * sample in the low nibble), updating `state`. */
ReplayStatus replay_sound_adpcm_encode(const int16_t *samples, size_t count,
                                       ReplaySoundAdpcmState *state,
                                       ReplayBuffer *out);

/* Append the 4-byte chunk state header for `state` to `out`. */
ReplayStatus replay_sound_adpcm_write_header(const ReplaySoundAdpcmState *state,
                                             ReplayBuffer *out);

/* Decode `count` 4-bit codes (packed as above) from `nibbles`, writing samples
 * to `out_samples`, updating `state`. Exposed for testing and verification. */
void replay_sound_adpcm_decode(const uint8_t *nibbles, size_t count,
                               ReplaySoundAdpcmState *state,
                               int16_t *out_samples);

#endif
