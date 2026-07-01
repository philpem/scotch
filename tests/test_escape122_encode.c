/* Encoder round-trip test for Escape 122 (PAL8). Core invariant: decoding an
 * encoded frame reproduces the encoder's own reconstruction exactly, for a
 * stateful intra+delta sequence; and re-encoding an already-representable frame
 * is lossless. Self-contained (no external movie needed). */

#include "replay/replay_escape122.h"
#include "replay/replay_escape122_enc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define NPX (W * H)

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

static uint8_t src[NPX], recon1[NPX], pal[768];
static uint8_t out[1 << 16];

static void make_frame(uint8_t *f, unsigned seed)
{
    int i;
    for (i = 0; i < NPX; i++) {
        seed = seed * 1103515245u + 12345u;
        f[i] = (uint8_t)(seed >> 24);
    }
}

int main(void)
{
    ReplayEsc122Enc *enc = replay_esc122enc_open(W, H);
    ReplayEsc122 *dec = replay_esc122_open(W, H);
    int f, i;

    if (enc == NULL || dec == NULL) { fprintf(stderr, "open failed\n"); return 1; }
    for (i = 0; i < 768; i++) pal[i] = (uint8_t)(i * 7 + 3);   /* some palette */

    /* A stateful sequence: each encoded frame must decode to exactly the
     * encoder's reconstruction (identical palette indices). */
    for (f = 0; f < 6; f++) {
        size_t len;
        const uint8_t *p = (f == 0) ? pal : NULL;        /* palette on frame 0 */
        make_frame(src, (unsigned)(f + 1) * 2654435761u);
        len = replay_esc122enc_frame(enc, src, p, out, sizeof out);
        CHECK(len > 0, "encoded a frame");
        CHECK(replay_esc122_decode(dec, out, len) >= 0, "decode ok");
        CHECK(memcmp(replay_esc122_frame(dec), replay_esc122enc_recon(enc),
                     NPX) == 0, "decoder indices == encoder reconstruction");
    }
    replay_esc122_close(dec);
    replay_esc122enc_close(enc);

    /* Re-encoding an already-representable frame is lossless. */
    {
        ReplayEsc122Enc *e1 = replay_esc122enc_open(W, H);
        ReplayEsc122 *d1 = replay_esc122_open(W, H);
        ReplayEsc122Enc *e2 = replay_esc122enc_open(W, H);
        ReplayEsc122 *d2 = replay_esc122_open(W, H);
        size_t l1, l2;
        make_frame(src, 12345u);
        l1 = replay_esc122enc_frame(e1, src, pal, out, sizeof out);
        replay_esc122_decode(d1, out, l1);
        memcpy(recon1, replay_esc122_frame(d1), NPX);        /* representable */
        l2 = replay_esc122enc_frame(e2, recon1, pal, out, sizeof out);
        replay_esc122_decode(d2, out, l2);
        CHECK(memcmp(recon1, replay_esc122_frame(d2), NPX) == 0,
              "re-encoding a representable frame is lossless");
        replay_esc122_close(d1); replay_esc122_close(d2);
        replay_esc122enc_close(e1); replay_esc122enc_close(e2);
    }

    if (failures == 0) printf("OK\n");
    return failures != 0;
}
