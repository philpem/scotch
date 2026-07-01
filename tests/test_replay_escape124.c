/* Unit test for the Escape 124 (RGB555) decoder. Builds a minimal one-superblock
 * (8x8) intra frame that sends a per-superblock codebook with one uniform entry,
 * paints it to all 16 macroblock slots via the 16-bit mask, and checks the
 * decoded RGB555 pixels. Then a mode-gated-out frame verifies persistence. */

#include "replay/replay_escape124.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* LSB-first bit writer into `bs` (the block bitstream, after the 8-byte header). */
static size_t bitpos;
static uint8_t bs[64];
static void pb(uint32_t v, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) bs[bitpos >> 3] |= (uint8_t)(1u << (bitpos & 7));
        bitpos++;
    }
}

/* Assemble one escape124 frame unit: [u32 flags][u32 size][bitstream]. */
static size_t build_frame(uint8_t *out, uint32_t flags)
{
    size_t nbytes = (bitpos + 7) / 8, i;
    uint32_t total = (uint32_t)(8 + nbytes);
    out[0] = (uint8_t)flags;       out[1] = (uint8_t)(flags >> 8);
    out[2] = (uint8_t)(flags >> 16); out[3] = (uint8_t)(flags >> 24);
    out[4] = (uint8_t)total;       out[5] = (uint8_t)(total >> 8);
    out[6] = (uint8_t)(total >> 16); out[7] = (uint8_t)(total >> 24);
    for (i = 0; i < nbytes; i++) out[8 + i] = bs[i];
    return 8 + nbytes;
}

int main(void)
{
    ReplayEsc124 *s = replay_esc124_open(8, 8);
    uint8_t frame[64];
    size_t flen;
    const uint16_t *fr;
    int i;
    /* RGB555, red high: R=10, G=20, B=5 -> 0x2A85; stored with bit 15 alpha. */
    const uint16_t color = (uint16_t)((10u << 10) | (20u << 5) | 5u);
    const uint16_t want = (uint16_t)(color | 0x8000u);

    if (s == NULL) { fprintf(stderr, "open failed\n"); return 1; }

    /* flags: mode gate (bit 23) + send the per-superblock codebook (bit 18). */
    memset(bs, 0, sizeof bs); bitpos = 0;
    pb(1, 4);              /* codebook depth = 1 -> size = num_sb<<1 = 2 entries  */
    pb(0, 4);              /* entry 0: mask 0 -> all 4 pixels = color0            */
    pb(color, 15);        /*          color0                                     */
    pb(0, 15);             /*          color1 (unused)                            */
    pb(0, 4); pb(0, 15); pb(0, 15);   /* entry 1 (unused)                         */
    pb(0, 1);              /* skip run -> 0 (this superblock is coded)            */
    pb(0, 1);              /* leading bit -> main loop (not the pattern path)     */
    pb(0, 1);              /* macroblock: no codebook switch                      */
    pb(0, 1);              /* block index 0 (depth 1)                             */
    pb(0xFFFF, 16);        /* mask: paint the block to all 16 slots               */
    pb(1, 1);              /* continue = 1 -> drop to the pattern check           */
    pb(1, 1);              /* sub = 1; flags&0x10000 == 0 -> sub-mode is a no-op  */

    flen = build_frame(frame, 0x800000u | 0x40000u);
    CHECK(replay_esc124_decode(s, frame, flen) == 0, "intra frame decoded");

    fr = replay_esc124_frame(s);
    for (i = 0; i < 64; i++)
        if (fr[i] != want) { CHECK(0, "all 64 pixels = color"); break; }

    /* A mode-gated-out frame (flags & 0x7800000 == 0) copies the previous frame. */
    bitpos = 0;               /* no bitstream */
    flen = build_frame(frame, 0u);
    CHECK(replay_esc124_decode(s, frame, flen) == 0, "gated-out frame decoded");
    fr = replay_esc124_frame(s);
    CHECK(fr[0] == want && fr[63] == want, "gated frame keeps previous pixels");

    replay_esc124_close(s);
    if (failures == 0) printf("OK\n");
    return failures != 0;
}
