#ifndef REPLAY_ESCAPE130_ENC_H
#define REPLAY_ESCAPE130_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encoder for Eidos/Acorn "Escape 2.0" (Replay video format 130). Produces chunks
 * the in-tree decoder (replay_escape130.c) reproduces exactly; format-compatible,
 * not bit-compatible with any Eidos encoder.
 *
 * Unlike the palettised/YUV555 codecs, a 130 block is a packed *state* word (a base
 * luma, a texture pattern, and a chroma pair -- see docs/spec/eidos-escape.md
 * § Type 130), and the state->RGB render is non-linear. So this encoder works at
 * the block-state level: given a frame's target block states -- the packed words
 * and per-block "textured" flags, exactly as the decoder exposes them
 * (replay_esc130_blocks / replay_esc130_textured) -- it emits the luma/chroma
 * prefix modes (SIGNS/COPY/DELTA/ABS) that reproduce each state from its left
 * neighbour, delta-coding across frames by skipping unchanged blocks.
 *
 * (Deriving block states from an arbitrary RGB source -- inverting the render --
 * is a separate analysis problem this encoder does not attempt.)
 */
typedef struct ReplayEsc130Enc ReplayEsc130Enc;

ReplayEsc130Enc *replay_esc130enc_open(unsigned width, unsigned height);
void replay_esc130enc_close(ReplayEsc130Enc *e);

/* Encode one frame from its block states: `word0` and `textured` are (width/2)*
 * (height/2) entries in raster order. Returns the chunk length, or 0 on error. */
size_t replay_esc130enc_frame(ReplayEsc130Enc *e, const uint32_t *word0,
                              const uint8_t *textured, uint8_t *out, size_t cap);

/* Encode one frame from an RGB888 source (width*height*3, the opened dimensions),
 * inverting the colour render: each 2x2 block is reduced to one colour and mapped
 * to the nearest representable flat block state. Lets arbitrary video be turned
 * into Escape 130. (Currently flat blocks only -- no per-block texture yet.)
 * Returns the chunk length, or 0 on error. */
size_t replay_esc130enc_frame_rgb(ReplayEsc130Enc *e, const uint8_t *rgb,
                                  uint8_t *out, size_t cap);

/* The reconstructed block-state array after the last frame. */
const uint32_t *replay_esc130enc_recon(const ReplayEsc130Enc *e);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE130_ENC_H */
