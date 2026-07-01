#ifndef REPLAY_ESCAPE100_H
#define REPLAY_ESCAPE100_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eidos/Acorn "Escape" video formats 100 and 102 (© Eidos plc 1993) -- the
 * earliest Escape codecs, decoded by the vendored Decomp100 / Decomp102 ARM
 * modules. This is a native, clean C reimplementation, reverse-engineered from
 * those modules; see docs/spec/eidos-escape.md (§ Type 100/102).
 *
 * The codec is a 5-bit-YUV, 2x2-block vector-quantiser: the 160x128 picture is a
 * grid of 2x2-pixel blocks, each pixel a 15-bit YUV555 word (Y in the low 5 bits,
 * chroma -- a combined U/V pair -- in bits 5..14). Chroma comes from a 256-entry
 * codebook. The picture persists across frames and is delta-coded: an escalating
 * skip-run VLC copies unmentioned blocks from the previous frame, a luma block
 * updates 1-2 luma values per its 3-bit selector mask, and a chroma block updates
 * only the chroma. 100 and 102 share the format and the codebook; they differ
 * only in the per-frame header (100: `u32 id=0x100`; 102: `u32 id=0x102` + one
 * reserved word).
 */
typedef struct ReplayEsc100 ReplayEsc100;

/* Open a decoder for a width x height frame (both even; the modules use 160x128).
 * NULL on allocation failure or bad dimensions. */
ReplayEsc100 *replay_esc100_open(unsigned width, unsigned height);
void replay_esc100_close(ReplayEsc100 *s);

/* Decode one frame. `frame` points at the frame's codec-id word (0x100 or 0x102);
 * `len` is the bytes available from there. Updates the persistent picture and
 * returns the number of bytes the frame consumed (word-aligned, so the caller can
 * step to the next frame in a multi-frame chunk), or 0 on a malformed frame. */
size_t replay_esc100_decode(ReplayEsc100 *s, const uint8_t *frame, size_t len);

/* The current picture: width*height YUV555 words (Y in bits 0..4, chroma 5..14). */
const uint16_t *replay_esc100_frame(const ReplayEsc100 *s);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE100_H */
