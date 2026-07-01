#ifndef REPLAY_ESCAPE122_ENC_H
#define REPLAY_ESCAPE122_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encoder for Eidos/Acorn "Escape 122" (Replay video format 122) -- a palettised
 * (PAL8) delta codec. Produces chunks the in-tree decoder (replay_escape122.c) and
 * the original DOS player decode; it is format-compatible, not bit-compatible with
 * any particular Eidos encoder. See docs/spec/eidos-escape.md (§ Type 122).
 *
 * The picture is 8-bit palette indices, decoded in 8x8 superblocks of 2x2
 * macroblocks; each macroblock is one palette index (uniform) or two indices with
 * a per-pixel mask. The frame and palette persist across frames: unchanged
 * superblocks are skipped, so the encoder keeps the reconstructed picture and
 * delta-codes against it.
 *
 * Input is one frame of `width*height` 8-bit palette indices. The palette is a
 * 256*3 array of 8-bit R,G,B; pass it on the first frame (and whenever it changes),
 * or NULL to keep the previous palette. Width and height must be multiples of 8.
 */
typedef struct ReplayEsc122Enc ReplayEsc122Enc;

ReplayEsc122Enc *replay_esc122enc_open(unsigned width, unsigned height);
void replay_esc122enc_close(ReplayEsc122Enc *e);

/* Encode one frame of palette indices into `out` (capacity `cap` bytes). Returns
 * the chunk length in bytes, or 0 on error / insufficient capacity. */
size_t replay_esc122enc_frame(ReplayEsc122Enc *e, const uint8_t *indices,
                              const uint8_t *palette, uint8_t *out, size_t cap);

/* The reconstructed picture after the last frame: width*height palette indices. */
const uint8_t *replay_esc122enc_recon(const ReplayEsc122Enc *e);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE122_ENC_H */
