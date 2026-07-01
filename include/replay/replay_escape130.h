#ifndef REPLAY_ESCAPE130_H
#define REPLAY_ESCAPE130_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eidos/Acorn "Escape 2.0" -- Replay video format 130. A YCbCr block codec: the
 * picture is a grid of 2x2-pixel blocks whose per-block state (a base luma, a
 * chroma pair, and an optional 2-bit-per-sub-pixel texture pattern) persists
 * across frames and is delta-coded; the display 2x upsamples that low-resolution
 * grid to the full frame with a separable blend, rendering textured blocks sharply.
 *
 * Two provenances (see docs/spec/eidos-escape.md § 130-spec and the .c files):
 *  - the bitstream DECODER (src/replay_escape130.c) is a clean-room implementation
 *    from the behavioural spec; no existing decoder was consulted;
 *  - the RENDER (src/dec130_render.c) is a hand-written reimplementation of
 *    DEC130.DLL's display path, reverse-engineered from the DLL and bit-exact to
 *    it. Together they reproduce DEC130.DLL's output bit-for-bit.
 *
 * Each ARMovie video chunk is a 16-byte header (`u16 magic=0x130`, flags, `u32
 * vsize`, reserved) followed by the LSB-first bitstream (`vsize-16` bytes); a
 * chunk with fewer than 16 bytes is a "no change" frame. Decode a chunk with
 * _decode(), then render the current picture to RGB888 with _render().
 */
typedef struct ReplayEsc130 ReplayEsc130;

/* Open a decoder for a width x height frame (both even). NULL on allocation
 * failure or bad dimensions. */
ReplayEsc130 *replay_esc130_open(unsigned width, unsigned height);
void replay_esc130_close(ReplayEsc130 *s);

/* Decode one chunk (16-byte header included), updating the persistent block
 * state. A chunk shorter than 16 bytes leaves the picture unchanged. Returns 0
 * on success, -1 on a NULL argument. */
int replay_esc130_decode(ReplayEsc130 *s, const uint8_t *chunk, size_t clen);

/* Render the current picture to a caller-supplied width*height*3 RGB888 buffer. */
void replay_esc130_render(ReplayEsc130 *s, uint8_t *rgb);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE130_H */
