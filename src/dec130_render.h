/*
 * dec130_render.h - bit-exact RGB565 render for Escape codec 130.
 *
 * Implemented in dec130_render.c, a hand-written reimplementation of DEC130.DLL's
 * display path (reverse-engineered from the DLL and bit-exact to it; see that
 * file's header).  This is the RE'd-from-DLL half of the codec; the clean-room
 * bitstream decoder lives in replay_escape130.c.
 */
#ifndef DEC130_RENDER_H
#define DEC130_RENDER_H
#include <stdint.h>

/*
 * One decoded 2x2-pixel block, in DEC130.DLL's ctx[2] layout:
 *   word0 : packed luma/chroma plus four 2-bit per-sub-pixel "texture" signs;
 *   word4 : the "expanded" colour -- three channels in disjoint lanes (green @
 *           bits 0..8, blue @ 10..18 half-scaled, red @ 20..28) -- plus a
 *           "textured" flag at bit 30.
 */
typedef struct { uint32_t word0, word4; } Dec130Block;

/*
 * Render a decoded codec-130 frame to RGB565.
 *   blocks : (W/2)*(H/2)+1 blocks; blocks[0] is the seed/sentinel, and real block
 *            (bx,by) is blocks[by*(W/2) + bx + 1].
 *   W,H    : frame dimensions (both even).
 *   out565 : W*H little-endian RGB565 pixels.
 */
void dec130_render(const Dec130Block *blocks, int W, int H, uint16_t *out565);

#endif
