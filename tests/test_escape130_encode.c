/* Encoder round-trip test for Escape 130. Core invariant: decoding an encoded
 * frame reproduces the encoder's own reconstruction (block states) exactly, for a
 * stateful intra + delta sequence of valid states, plus lossless re-encoding.
 * Self-contained -- constructs valid block states directly (flat blocks, and
 * textured blocks using a known sign-table pattern). */

#include "replay/replay_escape130.h"
#include "replay/replay_escape130_enc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define NB ((W / 2) * (H / 2))   /* 1024 blocks */

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

static uint32_t w0[NB];
static uint8_t  tx[NB];
static uint8_t  out[1 << 18];

/* Build a frame of valid block states. Each block is either a flat block (any
 * 6-bit luma + a clean 5-bit cb/cr) or a textured block (the sign-table entry for
 * sidx 1 lives in texture bits 8..15 as 0x06, an even base luma, a step). */
static void make_states(unsigned seed)
{
    int i;
    for (i = 0; i < NB; i++) {
        unsigned r;
        unsigned cb, cr;
        seed = seed * 1103515245u + 12345u; r = seed >> 8;
        cb = r & 0x1Fu; cr = (r >> 5) & 0x1Fu;
        if ((r >> 10) & 1u) {                            /* textured */
            unsigned step = (r >> 11) & 3u, ya = (r >> 13) & 0x1Fu;
            w0[i] = 0x0600u | (step << 6) | (ya << 1) | (cb << 16) | (cr << 24);
            tx[i] = 1;
        } else {                                         /* flat */
            unsigned yavg = (r >> 11) & 0x3Fu;
            w0[i] = yavg | (cb << 16) | (cr << 24);
            tx[i] = 0;
        }
    }
}

int main(void)
{
    ReplayEsc130Enc *enc = replay_esc130enc_open(W, H);
    ReplayEsc130 *dec = replay_esc130_open(W, H);
    int f;

    if (enc == NULL || dec == NULL) { fprintf(stderr, "open failed\n"); return 1; }
    CHECK(replay_esc130_block_count(dec) == NB, "block count");

    /* Stateful sequence: each encoded frame must decode to exactly the encoder's
     * reconstructed states (word0 + textured flags). Frames alternate fresh random
     * states and lightly-mutated ones (exercising skips and deltas). */
    for (f = 0; f < 6; f++) {
        size_t len;
        if (f % 2 == 0) make_states((unsigned)(f + 1) * 2654435761u);
        else { int i; for (i = 0; i < NB; i += 3) { w0[i] ^= 2u; } } /* nudge some */
        len = replay_esc130enc_frame(enc, w0, tx, out, sizeof out);
        CHECK(len > 0, "encoded a frame");
        CHECK(replay_esc130_decode(dec, out, len) == 0, "decode ok");
        CHECK(memcmp(replay_esc130_blocks(dec), replay_esc130enc_recon(enc),
                     NB * 4) == 0, "decoded states == encoder reconstruction");
    }
    replay_esc130_close(dec);
    replay_esc130enc_close(enc);

    /* Re-encoding an already-valid state array reproduces it losslessly. */
    {
        ReplayEsc130Enc *e2 = replay_esc130enc_open(W, H);
        ReplayEsc130 *d2 = replay_esc130_open(W, H);
        size_t len;
        make_states(777u);
        len = replay_esc130enc_frame(e2, w0, tx, out, sizeof out);
        replay_esc130_decode(d2, out, len);
        CHECK(memcmp(replay_esc130_blocks(d2), w0, NB * 4) == 0,
              "re-encode reproduces the states");
        replay_esc130_close(d2);
        replay_esc130enc_close(e2);
    }

    /* RGB inversion path: encoding an RGB frame yields a valid chunk that decodes
     * to the encoder's own reconstruction -- i.e. RGB -> states is emitted
     * faithfully, whether the encoder chose flat or textured blocks. */
    {
        ReplayEsc130Enc *er = replay_esc130enc_open(W, H);
        ReplayEsc130 *dr = replay_esc130_open(W, H);
        static uint8_t rgb[W * H * 3];
        size_t len; int y, x;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                int p = (y * W + x) * 3;
                rgb[p]     = (uint8_t)(x * 255 / (W - 1));
                rgb[p + 1] = (uint8_t)(y * 255 / (H - 1));
                rgb[p + 2] = (uint8_t)((x + y) * 255 / (W + H - 2));
            }
        len = replay_esc130enc_frame_rgb(er, rgb, out, sizeof out);
        CHECK(len > 0, "rgb frame encoded");
        CHECK(replay_esc130_decode(dr, out, len) == 0, "rgb chunk decodes");
        CHECK(memcmp(replay_esc130_blocks(dr), replay_esc130enc_recon(er),
                     NB * 4) == 0, "rgb-encoded frame decodes to the reconstruction");
        replay_esc130_close(dr);
        replay_esc130enc_close(er);
    }

    /* Texture path: a per-sub-pixel luma checkerboard (bright/dark within every 2x2
     * block) is exactly what a textured block captures and a flat block cannot, so
     * the encoder must choose textured blocks -- and they must still round-trip. */
    {
        ReplayEsc130Enc *er = replay_esc130enc_open(W, H);
        ReplayEsc130 *dr = replay_esc130_open(W, H);
        static uint8_t rgb[W * H * 3];
        const uint8_t *tex;
        size_t len; int y, x, i, ntex = 0;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                int p = (y * W + x) * 3;
                uint8_t v = (uint8_t)(((x ^ y) & 1) ? 230 : 30);
                rgb[p] = rgb[p + 1] = rgb[p + 2] = v;
            }
        len = replay_esc130enc_frame_rgb(er, rgb, out, sizeof out);
        CHECK(replay_esc130_decode(dr, out, len) == 0, "textured chunk decodes");
        CHECK(memcmp(replay_esc130_blocks(dr), replay_esc130enc_recon(er),
                     NB * 4) == 0, "textured rgb frame decodes to the reconstruction");
        tex = replay_esc130_textured(dr);
        for (i = 0; i < NB; i++) if (tex[i]) ntex++;
        CHECK(ntex > 0, "encoder chose textured blocks for a luma checkerboard");
        replay_esc130_close(dr);
        replay_esc130enc_close(er);
    }

    if (failures == 0) printf("OK\n");
    return failures != 0;
}
