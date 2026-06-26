/*
 * replay_escape_adpcm.c -- see include/replay/replay_escape_adpcm.h.
 *
 * Clean-room reconstruction of the 4-bit ADPCM decoder built into WINSTR.DLL
 * (the Replay "Streamer" engine used by Eidos Escape movies), from a static
 * analysis of the DLL. The routines and tables mirror, byte-for-byte in
 * behaviour, the code at these WINSTR RVAs (ImageBase 0x10000000):
 *
 *   0x10004840  mono ADPCM block decoder   (one running state)
 *   0x100049b0  stereo ADPCM block decoder (two running states)
 *   0x10011040  index_table[16]  (.data)   -- canonical IMA
 *   0x10011080  step_table[89]   (.data)   -- IMA with two altered entries
 *
 * Deliberate deviations from canonical IMA/DVI ADPCM (both present in WINSTR):
 *   1. Reconstruction omits the "step >> 3" bias:
 *          diff = ((code & 7) * step) >> 2
 *      (canonical IMA is diff = ((2*(code&7) + 1) * step) >> 3).
 *   2. step_table[62] = 2749 (canonical 2747) and step_table[63] = 3024
 *      (canonical 3022).
 */
#include "replay/replay_escape_adpcm.h"

/* index_table[16] -- identical to canonical IMA; entries 8..15 mirror 0..7 (the
 * sign bit does not change the step adaptation). */
const int replay_escape_adpcm_index_table[16] = {
    -1, -1, -1, -1,  2,  4,  6,  8,
    -1, -1, -1, -1,  2,  4,  6,  8,
};

/* step_table[89] -- canonical IMA except indices 62 and 63 (marked). */
const int16_t replay_escape_adpcm_step_table[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,
     2749, /* [62] WINSTR; canonical IMA = 2747 */
     3024, /* [63] WINSTR; canonical IMA = 3022 */
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

int replay_escape_adpcm_decode_nibble(ReplayEscapeAdpcmState *s, unsigned code)
{
    int step, index, diff, pred;

    code &= 0x0f;
    step = replay_escape_adpcm_step_table[s->index];

    /* Advance the step index using the current code, then clamp to 0..88. */
    index = s->index + replay_escape_adpcm_index_table[code];
    if (index < 0)        index = 0;
    else if (index > 88)  index = 88;
    s->index = index;

    /* WINSTR magnitude reconstruction: diff = ((code & 7) * step) >> 2, i.e. the
     * DLL's three conditional adds of 4*step / 2*step / 1*step before the >>2. */
    diff = (int)(((unsigned)(code & 7) * (unsigned)step) >> 2);

    pred = s->predicted;
    if (code & 8) pred -= diff;
    else          pred += diff;

    if (pred > 32767)       pred = 32767;
    else if (pred < -32768) pred = -32768;
    s->predicted = pred;

    return pred;
}

void replay_escape_adpcm_decode_mono(ReplayEscapeAdpcmState *s,
                                     const uint8_t *src, int16_t *dst,
                                     size_t nbytes)
{
    size_t i;
    for (i = 0; i < nbytes; i++) {
        uint8_t b = src[i];
        *dst++ = (int16_t)replay_escape_adpcm_decode_nibble(s, (b >> 4) & 0x0f);
        *dst++ = (int16_t)replay_escape_adpcm_decode_nibble(s,  b       & 0x0f);
    }
}

void replay_escape_adpcm_decode_stereo(ReplayEscapeAdpcmState *s0,
                                       ReplayEscapeAdpcmState *s1,
                                       const uint8_t *src, int16_t *dst,
                                       size_t nbytes)
{
    size_t i;
    for (i = 0; i < nbytes; i++) {
        uint8_t b = src[i];
        *dst++ = (int16_t)replay_escape_adpcm_decode_nibble(s0, (b >> 4) & 0x0f);
        *dst++ = (int16_t)replay_escape_adpcm_decode_nibble(s1,  b       & 0x0f);
    }
}
