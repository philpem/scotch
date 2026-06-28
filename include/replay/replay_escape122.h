#ifndef REPLAY_ESCAPE122_H
#define REPLAY_ESCAPE122_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eidos/Acorn "Escape 122" -- Replay video format 122. Despite the "Escape" name
 * it is a *completely different*, palettised (PAL8) codec from 124 (RGB555) and
 * 130 (YUV): the proprietary Windows Streamer DLLs cannot decode it (only the
 * original Eidos DOS player did). This decoder implements the format documented
 * in docs/spec/eidos-escape.md (NihAV's Escape122 decoder was a reference for the
 * format).
 *
 * Each ARMovie video chunk is `[u32 codec_id=0x116][u32 vsize][u16 pal_size]
 * [VGA palette: pal_size bytes, 3/entry, 6-bit][bitstream LSB-first]`. The frame
 * (width*height 8-bit palette indices) and palette persist across frames: 122 is
 * delta-coded in 8x8 superblocks; skipped superblocks keep their contents, and a
 * `pal_size == 0` chunk reuses the previous palette.
 */
typedef struct ReplayEsc122 ReplayEsc122;

/* Open a decoder for a width x height frame (both should be multiples of 8).
 * NULL on allocation failure or zero dimensions. */
ReplayEsc122 *replay_esc122_open(unsigned width, unsigned height);
void replay_esc122_close(ReplayEsc122 *s);

/* Decode one whole chunk (`chunk`/`clen` is the chunk's video region). Returns 1
 * for an intra/keyframe, 0 for a delta frame, -1 on a malformed chunk. The result
 * (and palette) is available via the accessors below. */
int replay_esc122_decode(ReplayEsc122 *s, const uint8_t *chunk, size_t clen);

const uint8_t *replay_esc122_frame(const ReplayEsc122 *s);   /* width*height idx */
const uint8_t *replay_esc122_palette(const ReplayEsc122 *s); /* 256*3 RGB (8-bit) */

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE122_H */
