/* Encoder round-trip test for Escape 100/102. The core invariant: for every
 * frame the encoder emits, decoding it must reproduce the encoder's own
 * reconstruction exactly (the encoder correctly models what the decoder yields),
 * and re-encoding an already-representable frame is lossless. Self-contained. */

#include "replay/replay_escape100.h"
#include "replay/replay_escape100_enc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 160
#define H 128
#define NPX (W * H)

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

static uint16_t src[NPX], recon1[NPX];
static uint8_t buf[1 << 17];

/* A deterministic pseudo-random YUV555 frame parameterised by seed. */
static void make_frame(uint16_t *f, unsigned seed)
{
    int i;
    for (i = 0; i < NPX; i++) {
        seed = seed * 1103515245u + 12345u;
        f[i] = (uint16_t)((seed >> 16) & 0x7FFFu);
    }
}

int main(void)
{
    ReplayEsc100Enc *e = replay_esc100enc_open(W, H, 0x100);
    ReplayEsc100 *d = replay_esc100_open(W, H);
    unsigned codec[2] = { 0x100, 0x102 };
    int ci, f;

    if (e == NULL || d == NULL) { fprintf(stderr, "open failed\n"); return 1; }
    replay_esc100enc_close(e);

    for (ci = 0; ci < 2; ci++) {
        ReplayEsc100Enc *enc = replay_esc100enc_open(W, H, codec[ci]);
        ReplayEsc100 *dec = replay_esc100_open(W, H);
        if (enc == NULL || dec == NULL) { fprintf(stderr, "open\n"); return 1; }

        /* A stateful sequence of frames (intra + deltas): each encoded frame must
         * decode to exactly the encoder's reconstruction. */
        for (f = 0; f < 6; f++) {
            size_t len;
            make_frame(src, (unsigned)(f + 1) * 2654435761u);
            len = replay_esc100enc_frame(enc, src, buf, sizeof buf);
            CHECK(len > 0, "encoded a frame");
            CHECK(replay_esc100_decode(dec, buf, len) == len, "decode consumes the frame");
            CHECK(memcmp(replay_esc100_frame(dec), replay_esc100enc_recon(enc),
                         NPX * 2) == 0, "decoder output == encoder reconstruction");
        }
        replay_esc100_close(dec);
        replay_esc100enc_close(enc);
    }

    /* Re-encoding an already-representable frame is lossless: encode arbitrary
     * data, decode to a representable frame, then encoding *that* and decoding
     * reproduces it byte-for-byte. */
    {
        ReplayEsc100Enc *e1 = replay_esc100enc_open(W, H, 0x100);
        ReplayEsc100 *d1 = replay_esc100_open(W, H);
        ReplayEsc100Enc *e2 = replay_esc100enc_open(W, H, 0x100);
        ReplayEsc100 *d2 = replay_esc100_open(W, H);
        size_t l1, l2;
        make_frame(src, 99u);
        l1 = replay_esc100enc_frame(e1, src, buf, sizeof buf);
        replay_esc100_decode(d1, buf, l1);
        memcpy(recon1, replay_esc100_frame(d1), NPX * 2);   /* representable */
        l2 = replay_esc100enc_frame(e2, recon1, buf, sizeof buf);
        replay_esc100_decode(d2, buf, l2);
        CHECK(memcmp(recon1, replay_esc100_frame(d2), NPX * 2) == 0,
              "re-encoding a representable frame is lossless");
        replay_esc100_close(d1); replay_esc100_close(d2);
        replay_esc100enc_close(e1); replay_esc100enc_close(e2);
    }

    if (failures == 0) printf("OK\n");
    return failures != 0;
}
