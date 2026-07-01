#ifndef REPLAY_ESCAPE100_ENC_H
#define REPLAY_ESCAPE100_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encoder for Eidos/Acorn "Escape" video formats 100 and 102. Produces a bitstream
 * that the in-tree decoder (replay_escape100.c) and the original Decomp100/102 ARM
 * modules decode correctly -- it is *not* bit-compatible with Eidos's MovieCompress
 * (encoder decisions are our own), only format-compatible.
 *
 * It is a 2x2-block vector quantiser over the fixed 256-entry chroma codebook: each
 * block takes one codebook chroma and 1-2 five-bit luma values selected per
 * sub-pixel. The encoder keeps the reconstructed picture and delta-codes: a block
 * whose reconstruction is unchanged from the previous frame is skipped. See
 * docs/spec/eidos-escape.md (§ Type 100/102).
 *
 * Input is YUV555 (Y in bits 0..4, U 5..9, V 10..14), the codec's own pixel format
 * -- the same words the decoder emits.
 */
typedef struct ReplayEsc100Enc ReplayEsc100Enc;

/* Open an encoder for width x height frames (both even). `codec_id` is 0x100 or
 * 0x102 (selects the frame header). NULL on bad arguments / allocation failure. */
ReplayEsc100Enc *replay_esc100enc_open(unsigned width, unsigned height,
                                       unsigned codec_id);
void replay_esc100enc_close(ReplayEsc100Enc *e);

/* Encode one YUV555 source frame into `out` (capacity `cap` bytes). Returns the
 * number of bytes written, or 0 on error / insufficient capacity. */
size_t replay_esc100enc_frame(ReplayEsc100Enc *e, const uint16_t *src,
                              uint8_t *out, size_t cap);

/* The reconstructed picture after the last encoded frame (what a decoder holds):
 * width*height YUV555 words. Useful for measuring encode error. */
const uint16_t *replay_esc100enc_recon(const ReplayEsc100Enc *e);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE100_ENC_H */
