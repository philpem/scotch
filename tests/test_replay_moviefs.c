/* Unit test for the MovieFS per-frame wrapper stripper. The wrapper layout was
 * reverse-engineered from a real type-602 (Cinepak) movie; this guards the
 * byte arithmetic (size = frame_len + 12, frame at +16, stride size + 4) and
 * the truncation handling, without needing the external codec modules. */

#include "replay/replay_moviefs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* Append a MovieFS-wrapped frame [size LE][flags][w LE][h LE][payload] to buf,
 * where size = payload_len + 12. Returns bytes written. */
static size_t put_frame(uint8_t *buf, const uint8_t *payload, size_t len,
                        unsigned w, unsigned h)
{
    uint32_t size = (uint32_t)len + 12u;
    size_t p = 0;
    buf[p++] = (uint8_t)(size);       buf[p++] = (uint8_t)(size >> 8);
    buf[p++] = (uint8_t)(size >> 16); buf[p++] = (uint8_t)(size >> 24);
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* flags */
    buf[p++] = (uint8_t)(w);  buf[p++] = (uint8_t)(w >> 8);
    buf[p++] = (uint8_t)(w >> 16); buf[p++] = (uint8_t)(w >> 24);
    buf[p++] = (uint8_t)(h);  buf[p++] = (uint8_t)(h >> 8);
    buf[p++] = (uint8_t)(h >> 16); buf[p++] = (uint8_t)(h >> 24);
    memcpy(buf + p, payload, len);
    return p + len;
}

int main(void)
{
    uint8_t in[512], out[512];
    size_t in_len = 0, out_len = 0;
    size_t frames;

    const uint8_t f0[] = { 0x00, 0x01, 0x02, 0x03, 0x04 };       /* 5 bytes */
    const uint8_t f1[] = { 0xAA, 0xBB, 0xCC };                   /* 3 bytes */
    const uint8_t f2[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 }; /* 6 bytes */

    in_len += put_frame(in + in_len, f0, sizeof f0, 160, 120);
    in_len += put_frame(in + in_len, f1, sizeof f1, 160, 120);
    in_len += put_frame(in + in_len, f2, sizeof f2, 160, 120);

    /* Each frame contributes 16 wrapper bytes + payload. */
    CHECK(in_len == (16 + 5) + (16 + 3) + (16 + 6), "wrapped length");

    frames = replay_moviefs_unwrap_chunk(in, in_len, out, &out_len);
    CHECK(frames == 3, "frame count");
    CHECK(out_len == sizeof f0 + sizeof f1 + sizeof f2, "stripped length");
    CHECK(memcmp(out, f0, sizeof f0) == 0, "frame 0 payload");
    CHECK(memcmp(out + sizeof f0, f1, sizeof f1) == 0, "frame 1 payload");
    CHECK(memcmp(out + sizeof f0 + sizeof f1, f2, sizeof f2) == 0,
          "frame 2 payload");

    /* A truncated trailing wrapper yields only the complete frames. */
    frames = replay_moviefs_unwrap_chunk(in, in_len - 4, out, &out_len);
    CHECK(frames == 2, "truncated frame count");
    CHECK(out_len == sizeof f0 + sizeof f1, "truncated stripped length");

    /* Garbage / too-small first wrapper yields zero frames. */
    {
        uint8_t bad[16] = { 4, 0, 0, 0 }; /* size = 4 < 12 */
        frames = replay_moviefs_unwrap_chunk(bad, sizeof bad, out, &out_len);
        CHECK(frames == 0 && out_len == 0, "bad wrapper rejected");
    }

    if (failures == 0)
        printf("OK\n");
    return failures != 0;
}
