/*
 * dec130_render.c - RGB565 render for the Escape codec 130 (DEC130.DLL display path).
 *
 * Provenance: this is a hand-written reimplementation of DEC130.DLL's render.  The
 * behaviour was reverse-engineered from the DLL (by probing its output as an oracle)
 * and this reproduces it bit-for-bit on every test movie.  The codec's clean-room
 * part is the bitstream decoder in escape130.c; this file is the RE'd render,
 * reproduced so the decoder can emit pixels identical to the reference player.
 *
 * Model.  The codec stores a low-resolution grid of 2x2-pixel blocks (see
 * escape130.c / docs/escape130-spec.md).  Each block carries two 32-bit words:
 *   word0 : packed luma/chroma plus four 2-bit per-sub-pixel "texture" signs;
 *   word4 : the "expanded" colour, three channels packed in separate lanes --
 *           green @ bits 0..8, blue @ 10..18 (half-scaled), red @ 20..28 -- plus
 *           flag bit 30 set when the block is "textured".
 * The display 2x upsamples that grid to full resolution.  For each output pixel:
 *   - a flat block in the interior is a separable (2,1,1)/4 blend of the block and
 *     its horizontal + vertical neighbours (a textured neighbour is not blended
 *     toward -- its term reverts to the block itself);
 *   - a textured block is rendered sharply: each sub-pixel gets its own luma step
 *     (+/-R_STEP) per its sign;
 *   - the frame border is special: the top and bottom scanlines carry no blend
 *     (just the block colour with a per-phase rounding bias), and the leftmost and
 *     rightmost pixel columns are the plain block colour.
 */
#include <stdint.h>
#include "dec130_render.h"

/* Per-stepidx luma texture step, packed the same way as word4 (SWAR add/sub). */
static const uint32_t R_STEP[4] = { 0x00801008u, 0x01002010u, 0x02805028u, 0x0500a050u };

/* Per-sub-pixel-phase rounding biases (phase = (oy&1)*2 + (ox&1)). Green and blue
 * share one table; red has its own (and a wider shift). */
static const uint32_t RND_GB[4] = { 0, 12, 8, 4 };
static const uint32_t RND_R [4] = { 24, 0, 8, 16 };

/* Per-lane saturating clamp applied after adding a texture step to a colour word
 * (transcribed from DEC130.DLL 0x10009860): each channel carries a guard bit above
 * its value; the high bit distinguishes overflow (-> saturate to max) from
 * underflow (-> clamp to 0). */
static uint32_t clamp_step(uint32_t a)
{
    if (a & 0x10020100u) {
        if (a & 0x10000000u) { if (a & 0x20000000u) a |= 0x0ff00000u; else a &= 0xf00fffffu; }
        if (a & 0x100u)      { if (a & 0x200u)      a |= 0x000000ffu; else a &= 0xffffff00u; }
        if (a & 0x20000u)    { if (a & 0x40000u)    a |= 0x0001fc00u; else a &= 0xfffe03ffu; }
    }
    return a;
}

/* Pack one expanded colour word (green@0, blue@10, red@20) to RGB565. */
static uint16_t pack565(uint32_t cv)
{
    return (uint16_t)(((cv >> 12) & 0xf81fu) | ((cv & 0xfcu) << 3));
}

/* Blend three expanded-colour words with weights (2,1,1)/4 and per-phase rounding,
 * then pack to RGB565.  The three lanes are summed in parallel; the /4 and the lane
 * widths are folded into the per-channel shifts (green/blue >>4, red >>5). */
static uint16_t blend565(uint32_t dom, uint32_t hn, uint32_t vn, int ph)
{
    uint32_t sum = 2u * dom + hn + vn;
    uint32_t g = ((( sum        & 0x3ffu) + RND_GB[ph]) >> 4) & 0x3fu;
    uint32_t b = ((((sum >> 10) & 0x3ffu) + RND_GB[ph]) >> 4) & 0x1fu;
    uint32_t r = ((((sum >> 20) & 0x3ffu) + RND_R [ph]) >> 5) & 0x1fu;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void dec130_render(const uint8_t *blkbuf, int W, int H, uint16_t *out565)
{
    int bw = W / 2;
    const uint32_t *blk = (const uint32_t *)blkbuf;   /* {word0,word4} pairs; [0]=seed */

    /* Accessors for block (bx,by): the seed entry occupies index 0, so real block i
     * lives at word index (i+1)*2. */
    #define WORD0(bx,by) (blk[(((long)(by)*bw + (bx)) + 1) * 2])
    #define WORD4(bx,by) (blk[(((long)(by)*bw + (bx)) + 1) * 2 + 1])
    #define TEXTURED(bx,by) ((WORD4(bx,by) & 0x40000000u) != 0)

    for (int oy = 0; oy < H; oy++) {
        for (int ox = 0; ox < W; ox++) {
            int bx = ox >> 1, by = oy >> 1;
            int px = ox & 1, py = oy & 1, ph = py * 2 + px;
            uint32_t dom = WORD4(bx, by);
            int top_bottom = (oy == 0 || oy == H - 1);
            int left_right = (ox == 0 || ox == W - 1);
            uint16_t c;

            if (left_right && !top_bottom) {
                /* Leftmost / rightmost pixel column: the plain block colour. */
                c = pack565(dom & 0x3fffffffu);
            } else if (TEXTURED(bx, by)) {
                /* Textured block: sharp sub-pixel = colour +/- step per its sign
                 * (sign bits at word0 bit 8+2*ph: 01 = +, 10 = -, 00 = none). */
                uint32_t w0 = WORD0(bx, by);
                uint32_t step = R_STEP[(w0 >> 6) & 3];
                uint32_t a = dom;
                if      (w0 & (1u << (8 + 2 * ph))) a = dom + step;   /* +1 */
                else if (w0 & (1u << (9 + 2 * ph))) a = dom - step;   /* -1 */
                c = pack565(clamp_step(a));
            } else {
                /* Flat block. */
                uint32_t d = dom & 0x3fffffffu;
                uint32_t hn, vn;
                if (top_bottom) {
                    /* Top/bottom scanline: no blend, just the colour + phase bias. */
                    hn = vn = d;
                } else {
                    /* Interior: blend toward the phase's horizontal and vertical
                     * neighbours; a textured neighbour is not blended toward. */
                    int nx = px ? bx + 1 : bx - 1;
                    int ny = py ? by + 1 : by - 1;
                    hn = TEXTURED(nx, by) ? d : (WORD4(nx, by) & 0x3fffffffu);
                    vn = TEXTURED(bx, ny) ? d : (WORD4(bx, ny) & 0x3fffffffu);
                }
                c = blend565(d, hn, vn, ph);
            }
            out565[oy * W + ox] = c;
        }
    }
    #undef WORD0
    #undef WORD4
    #undef TEXTURED
}
