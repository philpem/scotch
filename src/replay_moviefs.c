#include "replay/replay_moviefs.h"

#include <string.h>

void replay_frame_wrap_iter_init(ReplayFrameWrapIter *it, const uint8_t *data,
                                 size_t len, ReplayWrapKind kind)
{
    it->data = data;
    it->len = len;
    it->pos = 0;
    it->kind = kind;
}

int replay_frame_wrap_iter_next(ReplayFrameWrapIter *it, const uint8_t **frame,
                                size_t *frame_len, int *is_null)
{
    const uint8_t *p;
    uint32_t size;
    size_t overhead, data_len;

    if (it->kind == REPLAY_WRAP_NONE) {
        /* The whole remaining payload is a single frame (no wrapper). Yield it
         * once, then signal end of payload. */
        if (it->pos >= it->len)
            return 0;
        if (frame != NULL)
            *frame = it->data + it->pos;
        if (frame_len != NULL)
            *frame_len = it->len - it->pos;
        if (is_null != NULL)
            *is_null = 0;
        it->pos = it->len;
        return 1;
    }

    if (it->pos + 16 > it->len)
        return 0; /* no room for another wrapper header */

    p = it->data + it->pos;
    size = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);

    if (size == 0) {
        /* Null frame: repeat the previous frame. The header carries no codec
         * data; advance past the 16-byte wrapper. */
        it->pos += 16;
        if (frame != NULL)
            *frame = NULL;
        if (frame_len != NULL)
            *frame_len = 0;
        if (is_null != NULL)
            *is_null = 1;
        return 1;
    }

    /* size = frame_len + overhead, where overhead is 12 (MovieFS) or 28
     * (VideoFS). The codec frame is at +16; the next wrapper is at +16+len. */
    overhead = (it->kind == REPLAY_WRAP_VIDEOFS) ? 28u : 12u;
    if (size < overhead)
        return 0; /* malformed */
    data_len = (size_t)size - overhead;
    if (data_len == 0 || it->pos + 16 + data_len > it->len)
        return 0; /* malformed / runs past the payload */

    if (frame != NULL)
        *frame = p + 16;
    if (frame_len != NULL)
        *frame_len = data_len;
    if (is_null != NULL)
        *is_null = 0;
    it->pos += 16 + data_len;
    return 1;
}

size_t replay_moviefs_unwrap_chunk(const uint8_t *in, size_t in_len,
                                   uint8_t *out, size_t *out_len)
{
    ReplayFrameWrapIter it;
    const uint8_t *frame;
    size_t frame_len, written = 0, frames = 0;
    int is_null;

    replay_frame_wrap_iter_init(&it, in, in_len, REPLAY_WRAP_MOVIEFS);
    while (replay_frame_wrap_iter_next(&it, &frame, &frame_len, &is_null)) {
        if (is_null)
            continue; /* MovieFS streams seen so far don't use null frames */
        memcpy(out + written, frame, frame_len);
        written += frame_len;
        frames++;
    }

    if (out_len != NULL)
        *out_len = written;
    return frames;
}
