/*
 * dec130_render.c - RGB565 render for the Escape codec 130 (DEC130.DLL display path).
 *
 * Provenance: this is a hand-written reimplementation of DEC130.DLL's render.  The
 * behaviour was reverse-engineered from the DLL (by probing its output as an oracle)
 * and this reproduces it bit-for-bit on every test movie.  The codec's clean-room
 * part is the bitstream decoder in replay_escape130.c; this file is the RE'd render,
 * reproduced so the decoder can emit pixels identical to the reference player.
 *
 * Model.  The codec stores a low-resolution grid of 2x2-pixel blocks (see
 * replay_escape130.c / docs/spec/eidos-escape.md).  Each block carries word0
 * (luma/chroma + four 2-bit per-sub-pixel texture signs) and word4 (the expanded
 * colour, see below).  The display 2x upsamples that grid to full resolution:
 *   - a flat interior block is a separable (2,1,1)/4 blend of the block and its
 *     horizontal + vertical neighbours (a textured neighbour is not blended toward
 *     -- its term reverts to the block itself);
 *   - a textured block is sharp: each sub-pixel gets its own luma step per its sign;
 *   - the frame border is special -- the top/bottom scanlines carry no blend, and
 *     the leftmost/rightmost pixel columns are the plain block colour.
 */
#include <stdint.h>
#include "dec130_render.h"

/*
 * Colour arithmetic is SWAR: the three channels live in disjoint lanes of one
 * 32-bit word -- green @ bits 0..8, blue @ 10..18 (half-scaled), red @ 20..28 --
 * each with a guard bit just above its value, so a lane can be added/subtracted
 * and clamped independently of the others.
 */
#define COLOUR_MASK   0x3fffffffu    /* the three colour lanes (drops the flags) */
#define TEXTURED_FLAG 0x40000000u    /* word4 bit 30: block carries fresh texture */

/* Per-stepidx luma texture step, packed in the colour lanes (SWAR add/sub). */
static const uint32_t R_STEP[4] = { 0x00801008u, 0x01002010u, 0x02805028u, 0x0500a050u };

/* Per-sub-pixel-phase rounding bias for the blend (phase = (oy&1)*2 + (ox&1)).
 * Green and blue share a table; red has its own (and a wider final shift). */
static const uint32_t RND_GREEN_BLUE[4] = { 0, 12, 8, 4 };
static const uint32_t RND_RED[4]        = { 24, 0, 8, 16 };

/*
 * Saturating clamp of one packed channel after a texture step (DEC130.DLL
 * 0x10009860).  When the channel's `guard` bit is set the value left its range;
 * the `sign` bit just above it says which way -- set = overflow (saturate: OR in
 * `sat`), clear = underflow (clamp to 0: AND with `clear`).  Channels occupy
 * disjoint lanes, so the three clamps are independent and order-free.
 */
static uint32_t clamp_channel(uint32_t v, uint32_t guard, uint32_t sign,
                              uint32_t sat, uint32_t clear)
{
    if (v & guard)
        v = (v & sign) ? (v | sat) : (v & clear);
    return v;
}
static uint32_t clamp_step(uint32_t v)
{
    v = clamp_channel(v, 0x10000000u, 0x20000000u, 0x0ff00000u, 0xf00fffffu); /* red   */
    v = clamp_channel(v, 0x00000100u, 0x00000200u, 0x000000ffu, 0xffffff00u); /* green */
    v = clamp_channel(v, 0x00020000u, 0x00040000u, 0x0001fc00u, 0xfffe03ffu); /* blue  */
    return v;
}

/* Pack one expanded colour word (green@0, blue@10, red@20) to RGB565. */
static uint16_t pack565(uint32_t colour)
{
    return (uint16_t)(((colour >> 12) & 0xf81fu) | ((colour & 0xfcu) << 3));
}

/* (2,1,1)/4 blend of a dominant colour with its horizontal + vertical neighbours,
 * with per-phase rounding, packed to RGB565.  The three lanes are summed in
 * parallel; the /4 and the lane widths fold into the shifts (green/blue >>4,
 * red >>5). */
static uint16_t blend565(uint32_t dom, uint32_t horiz, uint32_t vert, int ph)
{
    uint32_t sum = 2u * dom + horiz + vert;
    uint32_t g = ((( sum        & 0x3ffu) + RND_GREEN_BLUE[ph]) >> 4) & 0x3fu;
    uint32_t b = ((((sum >> 10) & 0x3ffu) + RND_GREEN_BLUE[ph]) >> 4) & 0x1fu;
    uint32_t r = ((((sum >> 20) & 0x3ffu) + RND_RED[ph])        >> 5) & 0x1fu;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* One sub-pixel of a textured block: word4 +/- the block's luma step, selected by
 * the 2-bit sign at word0 bit (8 + 2*ph): 01 = +, 10 = -, 00 = none. */
static uint32_t textured_pixel(uint32_t word0, uint32_t word4, int ph)
{
    uint32_t step = R_STEP[(word0 >> 6) & 3];
    if      (word0 & (1u << (8 + 2 * ph))) return word4 + step;
    else if (word0 & (1u << (9 + 2 * ph))) return word4 - step;
    return word4;
}

/* Colour a flat neighbour contributes to the blend: its own colour, unless it is
 * textured, in which case the blend reverts to `self`. */
static uint32_t neighbour_colour(const Dec130Block *n, uint32_t self)
{
    return (n->word4 & TEXTURED_FLAG) ? self : (n->word4 & COLOUR_MASK);
}

void dec130_render(const Dec130Block *blocks, int W, int H, uint16_t *out565)
{
    int bw = W / 2;
    int oy, ox;
    /* Real block (bx,by) follows the seed at index 0. */
    #define BLOCK(bx, by) (blocks[((long)(by) * bw + (bx)) + 1])

    for (oy = 0; oy < H; oy++) {
        for (ox = 0; ox < W; ox++) {
            int bx = ox >> 1, by = oy >> 1;
            int px = ox & 1, py = oy & 1, ph = py * 2 + px;
            const Dec130Block *blk = &BLOCK(bx, by);
            uint32_t colour = blk->word4 & COLOUR_MASK;
            int edge_col = (ox == 0 || ox == W - 1);
            int edge_row = (oy == 0 || oy == H - 1);
            uint16_t c;

            if (edge_col && !edge_row) {
                /* Leftmost / rightmost pixel column: the plain block colour. */
                c = pack565(colour);
            } else if (blk->word4 & TEXTURED_FLAG) {
                /* Textured block: sharp per-sub-pixel luma step, then clamp. */
                c = pack565(clamp_step(textured_pixel(blk->word0, blk->word4, ph)));
            } else if (edge_row) {
                /* Top / bottom scanline: no neighbours, blend the colour with itself. */
                c = blend565(colour, colour, colour, ph);
            } else {
                /* Interior flat block: blend toward the phase's H and V neighbours. */
                const Dec130Block *hn = &BLOCK(px ? bx + 1 : bx - 1, by);
                const Dec130Block *vn = &BLOCK(bx, py ? by + 1 : by - 1);
                c = blend565(colour, neighbour_colour(hn, colour),
                             neighbour_colour(vn, colour), ph);
            }
            out565[oy * W + ox] = c;
        }
    }
    #undef BLOCK
}
