/* Unit test for the Eidos Escape (WINSTR.DLL) 4-bit ADPCM decoder.
 *
 * The mono vector is the one documented in the clean-room reconstruction
 * (rpl2avi/docs/escape-dll-analysis.md): the bytes 88 44 77 00 decode to
 * 0 0 7 16 35 75 75 75. This pins the WINSTR-specific maths (no step>>3 bias)
 * and the high-nibble-first ordering against the DLL. */

#include "replay/replay_escape_adpcm.h"

#include <stdint.h>
#include <stdio.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

int main(void)
{
    /* Mono reference vector. */
    {
        const uint8_t in[] = { 0x88, 0x44, 0x77, 0x00 };
        const int16_t want[] = { 0, 0, 7, 16, 35, 75, 75, 75 };
        int16_t out[8];
        size_t i;
        ReplayEscapeAdpcmState s;
        replay_escape_adpcm_init(&s);
        replay_escape_adpcm_decode_mono(&s, in, out, sizeof in);
        for (i = 0; i < 8; i++)
            CHECK(out[i] == want[i], "mono decode vector");
    }

    /* The two deliberate WINSTR step-table tweaks (vs canonical IMA). */
    CHECK(replay_escape_adpcm_step_table[62] == 2749, "step[62] = 2749");
    CHECK(replay_escape_adpcm_step_table[63] == 3024, "step[63] = 3024");
    CHECK(replay_escape_adpcm_step_table[0] == 7, "step[0] = 7");
    CHECK(replay_escape_adpcm_step_table[88] == 32767, "step[88] = 32767");
    CHECK(replay_escape_adpcm_index_table[4] == 2
       && replay_escape_adpcm_index_table[12] == 2, "index table mirror");

    /* Stereo splits each byte: high nibble -> left (s0), low -> right (s1).
     * With both states fresh, left should equal the mono decode of the high
     * nibbles and right the mono decode of the low nibbles. */
    {
        const uint8_t in[] = { 0x88, 0x44, 0x77, 0x00 };
        int16_t st[8];
        ReplayEscapeAdpcmState l, r, hi, lo;
        size_t i;
        replay_escape_adpcm_init(&l);
        replay_escape_adpcm_init(&r);
        replay_escape_adpcm_decode_stereo(&l, &r, in, st, sizeof in);
        replay_escape_adpcm_init(&hi);
        replay_escape_adpcm_init(&lo);
        for (i = 0; i < sizeof in; i++) {
            int wl = replay_escape_adpcm_decode_nibble(&hi, in[i] >> 4);
            int wr = replay_escape_adpcm_decode_nibble(&lo, in[i] & 0x0f);
            CHECK(st[i * 2] == wl, "stereo left = high nibble");
            CHECK(st[i * 2 + 1] == wr, "stereo right = low nibble");
        }
    }

    if (failures == 0)
        printf("OK\n");
    return failures != 0;
}
