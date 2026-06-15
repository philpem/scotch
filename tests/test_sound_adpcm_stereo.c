/* Verify replay_sound_adpcm_decode_stereo against the (separately tested) mono
 * decoder: a stereo frame's two interleaved nibbles must decode to the same
 * samples as decoding each channel's nibble stream on its own. */

#include "replay/replay_sound.h"

#include <stdint.h>
#include <stdio.h>

#define N 64

static int fails;

/* Decode `n` 4-bit codes as a mono stream (codes packed two per byte, low
 * nibble first) from a fresh state. */
static void mono_decode_codes(const unsigned char *codes, size_t n,
                              int16_t *out)
{
    unsigned char packed[(N + 1) / 2];
    ReplaySoundAdpcmState st = {0, 0};
    size_t i;
    for (i = 0; i < n; i++) {
        if ((i & 1U) == 0U)
            packed[i / 2] = codes[i];
        else
            packed[i / 2] |= (unsigned char)(codes[i] << 4);
    }
    replay_sound_adpcm_decode(packed, n, &st, out);
}

int main(void)
{
    unsigned char lcodes[N], rcodes[N], bytes[N];
    int16_t stereo_out[2 * N], lref[N], rref[N];
    ReplaySoundAdpcmState l = {0, 0}, r = {0, 0};
    size_t i;

    for (i = 0; i < N; i++) {
        lcodes[i] = (unsigned char)((i * 5 + 1) & 0xF);
        rcodes[i] = (unsigned char)((i * 3 + 7) & 0xF);
        bytes[i] = (unsigned char)(lcodes[i] | (rcodes[i] << 4)); /* L low, R high */
    }

    replay_sound_adpcm_decode_stereo(bytes, N, &l, &r, stereo_out);
    mono_decode_codes(lcodes, N, lref);
    mono_decode_codes(rcodes, N, rref);

    for (i = 0; i < N; i++) {
        if (stereo_out[2 * i] != lref[i]) {
            printf("FAIL left frame %zu: %d != %d\n", i,
                   stereo_out[2 * i], lref[i]);
            fails++;
        }
        if (stereo_out[2 * i + 1] != rref[i]) {
            printf("FAIL right frame %zu: %d != %d\n", i,
                   stereo_out[2 * i + 1], rref[i]);
            fails++;
        }
    }

    if (fails != 0) {
        printf("\n%d FAILURE(S)\n", fails);
        return 1;
    }
    printf("ok stereo ADPCM decode matches per-channel mono\n");
    return 0;
}
