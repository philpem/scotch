/* Unit test for the Escape 122 (PAL8) decoder. Builds a minimal one-superblock
 * frame (8x8) with an inline palette and a single broadcast macroblock, and
 * checks the decoded indices, the 6->8 palette expansion, and that a pal_size==0
 * delta frame reuses the palette. */

#include "replay/replay_escape122.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* LSB-first bit writer into `bs` (must be pre-zeroed). */
static size_t bitpos;
static uint8_t bs[64];
static void pb(uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1) bs[bitpos >> 3] |= (uint8_t)(1u << (bitpos & 7));
        bitpos++;
    }
}

/* Assemble a chunk: [0x116][vsize][pal_size][palette][bitstream]. */
static size_t build_chunk(uint8_t *out, unsigned pal_size, const uint8_t *pal,
                          const uint8_t *bits, size_t nbits)
{
    size_t p = 0, nbytes = (nbits + 7) / 8, i;
    out[0] = 0x16; out[1] = 0x01; out[2] = 0; out[3] = 0;        /* codec id 0x116 */
    p = 4;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 0;      /* vsize (unused) */
    out[p++] = (uint8_t)pal_size; out[p++] = (uint8_t)(pal_size >> 8);
    for (i = 0; i < pal_size; i++) out[p++] = pal[i];
    for (i = 0; i < nbytes; i++) out[p++] = bits[i];
    return p;
}

int main(void)
{
    ReplayEsc122 *s = replay_esc122_open(8, 8);
    uint8_t pal[33];                 /* 11 entries x 3, 6-bit components */
    uint8_t chunk[128];
    size_t clen;
    const uint8_t *fr, *p;
    int i;

    if (s == NULL) { fprintf(stderr, "open failed\n"); return 1; }

    /* palette: entry 10 = (20,30,40) in 6-bit; rest 0 */
    memset(pal, 0, sizeof pal);
    pal[10 * 3 + 0] = 20; pal[10 * 3 + 1] = 30; pal[10 * 3 + 2] = 40;

    /* bitstream: skip=0; pass A broadcasts one uniform-even block (idx 5 -> px 10)
     * to all 16 macroblocks; then exit pass A and skip pass B. */
    memset(bs, 0, sizeof bs); bitpos = 0;
    pb(0, 1);          /* read_ecode -> 0 (this superblock is coded) */
    pb(0, 1);          /* pass A while: 0 -> decode a block */
    pb(0, 4);          /* read_blk2x2 mask4 = 0 -> uniform even */
    pb(5, 7);          /* idx = 5 -> palette index 10 */
    pb(0xFFFF, 16);    /* mask: all 16 macroblocks */
    pb(1, 1);          /* pass A while: 1 -> stop */
    pb(1, 1);          /* pass B: 1 -> no pass B */

    clen = build_chunk(chunk, sizeof pal, pal, bs, bitpos);
    CHECK(replay_esc122_decode(s, chunk, clen) == 1, "intra frame decoded");

    fr = replay_esc122_frame(s);
    for (i = 0; i < 64; i++)
        if (fr[i] != 10) { CHECK(0, "all pixels = index 10"); break; }

    p = replay_esc122_palette(s);
    /* 6->8 expansion: v = (v<<2)|(v>>4): 20->81, 30->121, 40->162 */
    CHECK(p[10 * 3 + 0] == ((20 << 2) | (20 >> 4)), "palette R 6->8");
    CHECK(p[10 * 3 + 1] == ((30 << 2) | (30 >> 4)), "palette G 6->8");
    CHECK(p[10 * 3 + 2] == ((40 << 2) | (40 >> 4)), "palette B 6->8");

    /* A pal_size==0, all-skip delta frame keeps the previous frame and palette. */
    memset(bs, 0, sizeof bs); bitpos = 0;
    pb(1, 1); pb(0, 3);   /* read_ecode -> v3=0 -> skip = 1 (>0) for this sb */
    clen = build_chunk(chunk, 0, NULL, bs, bitpos);
    CHECK(replay_esc122_decode(s, chunk, clen) == 0, "delta frame");
    fr = replay_esc122_frame(s);
    CHECK(fr[0] == 10 && fr[63] == 10, "skip frame keeps previous pixels");

    replay_esc122_close(s);
    if (failures == 0) printf("OK\n");
    return failures != 0;
}
