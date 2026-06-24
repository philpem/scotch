#ifndef REPLAY_MOVIEFS_H
#define REPLAY_MOVIEFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * MovieFS (Warm Silence Software) re-encapsulated PC video codecs occupy Replay
 * video formats 600-699. Inside each Replay chunk, MovieFS wraps every codec
 * frame in a 16-byte per-frame header:
 *
 *     +0   uint32 LE  size   (counts the 12 header bytes after `size`, plus the
 *                             codec frame; i.e. size = codec_frame_len + 12)
 *     +4   uint32     flags
 *     +8   uint32 LE  width
 *     +12  uint32 LE  height
 *     +16  ...        the raw PC-codec frame (size - 12 bytes)
 *
 * The next frame's header follows at +(size + 4), i.e. +(16 + codec_frame_len).
 * Acorn Replay codecs have no such per-frame wrapper, so the compiled MovieFS
 * decompressor modules must be fed the unwrapped codec frames.
 *
 * See docs/moviefs-nut-passthrough.md.
 */

/*
 * Strip the MovieFS per-frame wrappers from one chunk's video payload
 * (`in`/`in_len`), copying the concatenated raw codec frames into `out`. `out`
 * must have capacity >= in_len (the stripped output is always smaller). On
 * success *out_len receives the stripped byte length and the return value is the
 * number of codec frames recovered. Parsing stops at the first malformed
 * wrapper (a size < 12 or a frame that would run past the payload), so a
 * truncated final wrapper yields the frames decoded so far; the return value is
 * 0 only when no valid frame is present at all.
 */
size_t replay_moviefs_unwrap_chunk(const uint8_t *in, size_t in_len,
                                   uint8_t *out, size_t *out_len);

#endif /* REPLAY_MOVIEFS_H */
