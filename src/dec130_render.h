/*
 * dec130_render.h - bit-exact RGB565 render for Escape codec 130.
 *
 * Implemented in dec130_render.c, a hand-written reimplementation of DEC130.DLL's
 * display path (reverse-engineered from the DLL and bit-exact to it; see that
 * file's header).  This is the RE'd-from-DLL half of the codec; the clean-room
 * bitstream decoder lives in escape130.c.
 */
#ifndef DEC130_RENDER_H
#define DEC130_RENDER_H
#include <stdint.h>

/*
 * Render a decoded codec-130 frame to RGB565.
 *   blkbuf : persistent block buffer, (W/2)*(H/2)+1 entries of 8 bytes
 *            { uint32 word0; uint32 word4; } in DEC130.DLL's ctx[2] layout
 *            (entry 0 is the seed/sentinel; real blocks follow).
 *   W,H    : frame dimensions (both even).
 *   out565 : W*H little-endian RGB565 pixels.
 */
void dec130_render(const uint8_t *blkbuf, int W, int H, uint16_t *out565);

#endif
