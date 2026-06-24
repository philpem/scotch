#ifndef REPLAY_MOVIEFS_H
#define REPLAY_MOVIEFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * MovieFS (Warm Silence Software, Replay video formats 600-699) and VideoFS
 * (Innovative Media Solutions, formats 900-999) both wrap each encapsulated PC
 * codec frame in a 16-byte per-frame header inside a Replay chunk:
 *
 *     +0   uint32 LE  size
 *     +4   uint32     flags  (0)
 *     +8   uint32 LE  width
 *     +12  uint32 LE  height
 *     +16  ...        the raw PC-codec frame
 *
 * The two differ only in what the `size` word counts (reverse-engineered from the
 * decompressor sources -- see docs/moviefs-nut-passthrough.md):
 *
 *     kind        size word        codec frame length   stride to next frame
 *     MovieFS     frame_len + 12   size - 12            size + 4   (= 16 + len)
 *     VideoFS     frame_len + 28   size - 28            size - 12  (= 16 + len)
 *
 * A size word of 0 marks a null frame (repeat the previous frame). Acorn Replay
 * codecs have no such per-frame wrapper.
 */

typedef enum {
    REPLAY_WRAP_MOVIEFS = 0, /* size = frame_len + 12 (600-699) */
    REPLAY_WRAP_VIDEOFS = 1  /* size = frame_len + 28 (900-999) */
} ReplayWrapKind;

/*
 * Forward iterator over the wrapped frames in one chunk's video payload.
 * Initialise with replay_frame_wrap_iter_init, then call ..._next until it
 * returns 0 (end of payload or a malformed wrapper).
 */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    ReplayWrapKind kind;
} ReplayFrameWrapIter;

void replay_frame_wrap_iter_init(ReplayFrameWrapIter *it, const uint8_t *data,
                                 size_t len, ReplayWrapKind kind);

/*
 * Yield the next codec frame. On success returns 1 and sets `frame` and
 * `frame_len` to the raw codec frame (pointing into the original payload). A
 * null frame (repeat-previous marker) sets `is_null` to 1 with `frame_len` 0;
 * otherwise `is_null` is 0. Returns 0 at end of payload or on a malformed
 * wrapper. Any of the out-parameters may be NULL if not needed.
 */
int replay_frame_wrap_iter_next(ReplayFrameWrapIter *it, const uint8_t **frame,
                                size_t *frame_len, int *is_null);

/*
 * Strip the MovieFS per-frame wrappers from one chunk's video payload
 * (`in`/`in_len`), copying the concatenated raw codec frames into `out`. `out`
 * must have capacity >= in_len. On success *out_len receives the stripped byte
 * length and the return value is the number of (non-null) codec frames. Stops at
 * the first malformed wrapper.
 */
size_t replay_moviefs_unwrap_chunk(const uint8_t *in, size_t in_len,
                                   uint8_t *out, size_t *out_len);

#endif /* REPLAY_MOVIEFS_H */
