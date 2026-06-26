#ifndef REPLAY_ESCAPE_ADPCM_H
#define REPLAY_ESCAPE_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eidos "Escape"/Replay (WINSTR.DLL) ADPCM sound decoder -- the codec behind
 * Replay sound format 101 ("standard" / "linear unsigned"), used by the Eidos
 * Escape movies (video formats 122/130). Clean-room reconstruction from a static
 * analysis of WINSTR.DLL (2000-02-18); see docs/spec/armovie-sound.md.
 *
 * It is an IMA/DVI-style 4-bit ADPCM but deliberately NOT bit-exact to canonical
 * IMA: the reconstruction step omits the usual `step >> 3` bias term, and two
 * step-table entries are altered ([62]=2749, [63]=3024). Decoding with the
 * canonical IMA maths instead makes the audio drift.
 *
 * State is zero-initialised once at stream start and runs continuously for the
 * whole movie -- WINSTR does NOT reset it at chunk boundaries and there is no
 * per-chunk state header (unlike Acorn sound format 2).
 */
typedef struct {
    int predicted;   /* last reconstructed sample, signed 16-bit range */
    int index;       /* step-table index, 0..88 */
} ReplayEscapeAdpcmState;

static inline void replay_escape_adpcm_init(ReplayEscapeAdpcmState *s)
{
    s->predicted = 0;
    s->index = 0;
}

/* Decode one 4-bit code (low nibble of `code`); updates *s, returns the
 * reconstructed signed 16-bit sample. */
int replay_escape_adpcm_decode_nibble(ReplayEscapeAdpcmState *s, unsigned code);

/* Mono: `nbytes` input bytes -> 2*nbytes samples in `dst`. Each byte holds two
 * codes; the HIGH nibble is the earlier sample. */
void replay_escape_adpcm_decode_mono(ReplayEscapeAdpcmState *s,
                                     const uint8_t *src, int16_t *dst,
                                     size_t nbytes);

/* Stereo: `nbytes` input bytes -> nbytes interleaved L/R frames (2*nbytes
 * samples). Per byte the HIGH nibble is the left sample (decoded with `s0`) and
 * the LOW nibble the right (decoded with `s1`); each channel keeps its own
 * running state. */
void replay_escape_adpcm_decode_stereo(ReplayEscapeAdpcmState *s0,
                                       ReplayEscapeAdpcmState *s1,
                                       const uint8_t *src, int16_t *dst,
                                       size_t nbytes);

/* The two tables, exposed for tests. */
extern const int16_t replay_escape_adpcm_step_table[89];
extern const int     replay_escape_adpcm_index_table[16];

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE_ADPCM_H */
