#ifndef REPLAY_ESCAPE124_H
#define REPLAY_ESCAPE124_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eidos/Acorn "Escape" video format 124 -- the codec decoded by WINSDEC.DLL
 * (`SC_Frame`) / EDEC.DLL (`EC_Frame`), used by Eidos DOS/RISC OS games. It is an
 * RGB555 block codec: an 8x8-superblock grid of 2x2 macroblocks, three rotating
 * macroblock codebooks (4-bit mask + two RGB555 colours per entry), an escalating
 * skip-run VLC, and per-superblock mask / pattern placement passes.
 *
 * This decoder is a reimplementation of the algorithm documented in
 * docs/spec/eidos-escape.md (§ Type 124), reverse-engineered from WINSDEC.DLL and
 * cross-referenced with the publicly documented FFmpeg `escape124` algorithm shape
 * (the 0x7800000 frame-flag gate, the mask_matrix, the codebook-switch transition
 * table, the codebook-flag bits and the skip VLC). The ARMovie variant differs
 * from stock escape124: it reads LSB-first, swaps the transition-table columns,
 * and adds the 17-bit mask+continue field and the pattern-placement path. It also
 * reproduces a genuine WINSDEC bit-reader quirk (a stale dword look-ahead across
 * 32-bit boundaries) so its output matches the shipping decoder bit-for-bit.
 *
 * Frame model: the decoder holds an RGB555 frame that persists across frames;
 * skipped superblocks keep their previous contents. Each call to _decode() takes
 * one *frame* -- the escape124 payload beginning with `[u32 flags][u32 size]`
 * followed by the LSB-first block bitstream. In real codec-124 movies a single
 * ARMovie video chunk concatenates several such frames back to back (see the
 * "frames per chunk" header field); the caller walks them by `size`.
 */
typedef struct ReplayEsc124 ReplayEsc124;

/* Open a decoder for a width x height frame (both multiples of 8). NULL on
 * allocation failure or zero/misaligned dimensions. */
ReplayEsc124 *replay_esc124_open(unsigned width, unsigned height);
void replay_esc124_close(ReplayEsc124 *s);

/* Decode one frame. `frame`/`len` is a single escape124 frame unit: the 32-bit
 * flags word, the 32-bit size, then the block bitstream. Returns 0 on success,
 * -1 on a malformed frame (too short). A frame whose flags clear the mode gate is
 * a full copy of the previous frame. The result is available via _frame(). */
int replay_esc124_decode(ReplayEsc124 *s, const uint8_t *frame, size_t len);

/* The current frame: width*height RGB555 pixels (0RRRRRGGGGGBBBBB, red high),
 * one uint16 per pixel. Bit 15 may be set on codec-written pixels; mask 0x7fff
 * for pure colour. */
const uint16_t *replay_esc124_frame(const ReplayEsc124 *s);

#ifdef __cplusplus
}
#endif
#endif /* REPLAY_ESCAPE124_H */
