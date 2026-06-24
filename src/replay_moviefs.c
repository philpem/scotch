#include "replay/replay_moviefs.h"

#include <string.h>

size_t replay_moviefs_unwrap_chunk(const uint8_t *in, size_t in_len,
                                   uint8_t *out, size_t *out_len)
{
    size_t pos = 0, written = 0, frames = 0;

    while (pos + 16 <= in_len) {
        uint32_t size = (uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8)
                      | ((uint32_t)in[pos + 2] << 16)
                      | ((uint32_t)in[pos + 3] << 24);
        size_t frame_len;
        if (size < 12)
            break; /* size must cover at least the 12 trailing header bytes */
        frame_len = (size_t)size - 12;
        if (frame_len == 0 || pos + 16 + frame_len > in_len)
            break;
        memcpy(out + written, in + pos + 16, frame_len);
        written += frame_len;
        frames++;
        pos += 16 + frame_len;
    }

    if (out_len != NULL)
        *out_len = written;
    return frames;
}
