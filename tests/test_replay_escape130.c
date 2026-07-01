/* Unit test for the Escape 130 decoder. Builds a minimal one-block (2x2) frame
 * with a single ABS-coded block and checks that decode + render run, change the
 * picture from the initial (all-zero) state, are deterministic, and that a
 * "no change" chunk (< 16 bytes) preserves the picture. Byte-exact agreement
 * with the reference decoder on real movies is covered separately. */

#include "replay/replay_escape130.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* LSB-first bit writer into the bitstream `bs`. */
static size_t bitpos;
static uint8_t bs[32];
static void pb(uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) bs[bitpos >> 3] |= (uint8_t)(1u << (bitpos & 7));
        bitpos++;
    }
}

/* Assemble a chunk: 16-byte header (magic 0x130) + the bitstream. */
static size_t build_chunk(uint8_t *out)
{
    size_t nbytes = (bitpos + 7) / 8, i;
    memset(out, 0, 16);
    out[0] = 0x30; out[1] = 0x01;          /* u16 magic = 0x130 */
    for (i = 0; i < nbytes; i++) out[16 + i] = bs[i];
    return 16 + nbytes;
}

int main(void)
{
    ReplayEsc130 *s = replay_esc130_open(2, 2);
    uint8_t chunk[64];
    uint8_t rgb0[2 * 2 * 3], rgb1[2 * 2 * 3], rgb2[2 * 2 * 3], rgb3[2 * 2 * 3];
    size_t clen;

    if (s == NULL) { fprintf(stderr, "open failed\n"); return 1; }

    /* Initial (all-zero block) render. */
    replay_esc130_render(s, rgb0);

    /* One block, ABS luma (yavg=32) + ABS chroma (cb=20, cr=12). */
    memset(bs, 0, sizeof bs); bitpos = 0;
    pb(1, 1);                 /* initial skip run -> 0 (code block 0 now)      */
    pb(0, 1); pb(1, 1); pb(1, 1);   /* luma prefix 011 -> ABS                  */
    pb(32, 6);                /* yavg = 32                                     */
    pb(1, 1); pb(1, 1);       /* chroma prefix 11 -> ABS                       */
    pb(20, 5); pb(12, 5);     /* cb = 20, cr = 12                              */
    pb(1, 1);                 /* trailing skip run -> 0 (loop then ends)       */

    clen = build_chunk(chunk);
    CHECK(replay_esc130_decode(s, chunk, clen) == 0, "chunk decoded");
    replay_esc130_render(s, rgb1);
    CHECK(memcmp(rgb0, rgb1, sizeof rgb0) != 0, "decode changed the picture");

    /* Determinism: rendering again yields the same pixels. */
    replay_esc130_render(s, rgb2);
    CHECK(memcmp(rgb1, rgb2, sizeof rgb1) == 0, "render is deterministic");

    /* A "no change" chunk (< 16 bytes) keeps the previous picture. */
    CHECK(replay_esc130_decode(s, chunk, 8) == 0, "no-change chunk decoded");
    replay_esc130_render(s, rgb3);
    CHECK(memcmp(rgb1, rgb3, sizeof rgb1) == 0, "no-change chunk keeps picture");

    replay_esc130_close(s);
    if (failures == 0) printf("OK\n");
    return failures != 0;
}
