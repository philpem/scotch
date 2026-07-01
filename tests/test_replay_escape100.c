/* Unit test for the Escape 100/102 decoder. Builds a minimal 160x128 frame that
 * codes one uniform luma block (top-left) then skips to the end, and checks the
 * decoded YUV555 pixels and the chroma-codebook lookup. Byte-exact agreement with
 * the Decomp100/102 modules is covered by the differential test. */

#include "replay/replay_escape100.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* LSB-first bit writer into the bitstream `bs`. */
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

int main(void)
{
    ReplayEsc100 *s = replay_esc100_open(160, 128);
    uint8_t frame[80];
    size_t flen, used, i;
    const uint16_t *px;
    /* codebook entry 0 is 0x77a0 (Y=0); luma 10 -> pixel 0x77aa */
    const uint16_t want = (uint16_t)(0x77a0u | 10u);

    if (s == NULL) { fprintf(stderr, "open failed\n"); return 1; }

    memset(bs, 0, sizeof bs); bitpos = 0;
    pb(0, 1);          /* initial skip run = 0 -> first block is at (0,0)       */
    /* block (0,0): luma mode, uniform (selector 0), luma A = 10, new chroma = 0. */
    pb(1, 1);          /* mode: 1 = luma                                        */
    pb(0, 3);          /* selector 0 -> all four sub-pixels use luma A          */
    pb(10, 5);         /* luma A = 10 (no luma B, since selector is 0)          */
    pb(1, 1);          /* new-chroma flag = 1 -> read a chroma index            */
    pb(0, 6);          /* chroma index 0 -> codebook[0] = 0x77a0                */
    /* then skip to the end: run 5119 pushes the cursor past block-row 64. */
    pb(1, 1); pb(7, 3); pb(127, 7); pb(4984, 15);   /* skip = 135 + 4984 = 5119 */

    /* assemble the frame: [u32 id=0x100][bitstream] */
    frame[0] = 0x00; frame[1] = 0x01; frame[2] = 0x00; frame[3] = 0x00;
    memcpy(frame + 4, bs, (bitpos + 7) / 8);
    flen = 4 + (bitpos + 7) / 8;

    used = replay_esc100_decode(s, frame, flen);
    CHECK(used > 0, "frame decoded (non-zero length)");

    px = replay_esc100_frame(s);
    /* the 2x2 block at (0,0): pixels (0,0),(1,0),(0,1),(1,1) */
    CHECK(px[0] == want && px[1] == want && px[160] == want && px[161] == want,
          "uniform luma block = codebook[0] | luma");
    /* a skipped pixel further in stays 0 (previous frame). */
    CHECK(px[160 * 64] == 0, "skipped region keeps the previous frame");

    /* a second, gated-out frame: an unknown id returns 0 and changes nothing. */
    frame[0] = 0x55;
    CHECK(replay_esc100_decode(s, frame, flen) == 0, "unknown id rejected");
    px = replay_esc100_frame(s);
    CHECK(px[0] == want, "rejected frame keeps the picture");

    replay_esc100_close(s);
    (void)i;
    if (failures == 0) printf("OK\n");
    return failures != 0;
}
