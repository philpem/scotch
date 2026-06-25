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

/* Append a wrapped frame [size LE][flags][w LE][h LE][payload] to buf, where
 * size = payload_len + overhead (12 for MovieFS, 28 for VideoFS). A payload_len
 * of 0 with overhead 0 emits a null-frame header (size word 0). Returns bytes
 * written. */
static size_t put_wrapped(uint8_t *buf, const uint8_t *payload, size_t len,
                          unsigned w, unsigned h, uint32_t overhead)
{
    uint32_t size = len == 0 && overhead == 0 ? 0u : (uint32_t)len + overhead;
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

    /* --- MovieFS (size = len + 12): concatenated unwrap --- */
    in_len += put_wrapped(in + in_len, f0, sizeof f0, 160, 120, 12);
    in_len += put_wrapped(in + in_len, f1, sizeof f1, 160, 120, 12);
    in_len += put_wrapped(in + in_len, f2, sizeof f2, 160, 120, 12);

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

    /* --- iterator: MovieFS pointers into the source --- */
    {
        ReplayFrameWrapIter it;
        const uint8_t *fr; size_t fl; int nul; int n = 0;
        const uint8_t *want[3] = { in + 16, in + 16 + (16 + 5),
                                   in + 16 + (16 + 5) + (16 + 3) };
        size_t wantlen[3] = { 5, 3, 6 };
        replay_frame_wrap_iter_init(&it, in, in_len, REPLAY_WRAP_MOVIEFS);
        while (replay_frame_wrap_iter_next(&it, &fr, &fl, &nul)) {
            CHECK(n < 3 && !nul && fr == want[n] && fl == wantlen[n],
                  "moviefs iter frame");
            n++;
        }
        CHECK(n == 3, "moviefs iter count");
    }

    /* --- VideoFS (size = len + 28): a basic two-frame stride --- */
    {
        uint8_t v[256]; size_t vl = 0;
        ReplayFrameWrapIter it;
        const uint8_t *fr; size_t fl; int nul; int n = 0;
        vl += put_wrapped(v + vl, f0, sizeof f0, 160, 120, 28);
        vl += put_wrapped(v + vl, f2, sizeof f2, 160, 120, 28);
        CHECK(vl == (16 + 5) + (16 + 6), "videofs wrapped length");
        replay_frame_wrap_iter_init(&it, v, vl, REPLAY_WRAP_VIDEOFS);
        while (replay_frame_wrap_iter_next(&it, &fr, &fl, &nul)) {
            CHECK(!nul, "videofs not null");
            if (n == 0) CHECK(fr == v + 16 && fl == 5, "videofs frame 0");
            else CHECK(fl == 6, "videofs frame 2");
            n++;
        }
        CHECK(n == 2, "videofs iter count");
    }

    /* --- Zero-word inter-frame padding is skipped (issue #39: real MovieFS
     * Cinepak movies pad between frames with 0/4/16 zero bytes -- not a 16-byte
     * "null frame", so a 4-byte gap must not be mistaken for a wrapper). --- */
    {
        uint8_t v[256]; size_t vl = 0;
        ReplayFrameWrapIter it;
        const uint8_t *fr; size_t fl; int nul; int n = 0;
        size_t f2_at;
        vl += put_wrapped(v + vl, f0, sizeof f0, 160, 88, 12);
        memset(v + vl, 0, 4); vl += 4;                       /* 4-byte pad */
        f2_at = vl + 16;
        vl += put_wrapped(v + vl, f2, sizeof f2, 160, 88, 12);
        memset(v + vl, 0, 16); vl += 16;                     /* 16-byte pad */
        replay_frame_wrap_iter_init(&it, v, vl, REPLAY_WRAP_MOVIEFS);
        while (replay_frame_wrap_iter_next(&it, &fr, &fl, &nul)) {
            CHECK(!nul, "padding never yields a null");
            if (n == 0) CHECK(fr == v + 16 && fl == 5, "pad: frame 0");
            else CHECK(fr == v + f2_at && fl == 6, "pad: frame 1 after 4-byte gap");
            n++;
        }
        CHECK(n == 2, "padding skipped, two real frames (trailing pad ignored)");
    }

    /* --- WRAP_NONE: the whole payload is one frame (Eidos Escape 130) --- */
    {
        ReplayFrameWrapIter it;
        const uint8_t *fr; size_t fl; int nul; int n = 0;
        const uint8_t whole[] = { 0x30, 0x01, 0x01, 0x80, 0x60, 0x35,
                                  0xAA, 0xBB, 0xCC, 0xDD };
        replay_frame_wrap_iter_init(&it, whole, sizeof whole, REPLAY_WRAP_NONE);
        while (replay_frame_wrap_iter_next(&it, &fr, &fl, &nul)) {
            CHECK(n == 0 && !nul && fr == whole && fl == sizeof whole,
                  "wrap_none whole payload");
            n++;
        }
        CHECK(n == 1, "wrap_none single frame");
        /* An empty payload yields no frames. */
        replay_frame_wrap_iter_init(&it, whole, 0, REPLAY_WRAP_NONE);
        CHECK(!replay_frame_wrap_iter_next(&it, &fr, &fl, &nul),
              "wrap_none empty");
    }

    if (failures == 0)
        printf("OK\n");
    return failures != 0;
}
