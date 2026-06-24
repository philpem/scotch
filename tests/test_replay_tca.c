/* Unit test for the TCA/ACEF decoder. Builds tiny synthetic IotaFilms (a 2x2,
 * mode-28 film: ACEF + PALE) exercising the raw, LZW and Delta paths, and checks
 * the decoded palette indices and the parsed palette. The format and the LZW
 * coding are re-derived here independently of replay_tca.c. */

#include "replay/replay_tca.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

#define W 2u
#define H 2u
#define FRM (W * H)   /* 4 bytes per frame (mode 28, 8bpp) */

static void put_u32(uint8_t *b, size_t o, uint32_t v)
{
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v>>8);
    b[o+2] = (uint8_t)(v>>16); b[o+3] = (uint8_t)(v>>24);
}

/* Append LSB-first bits. */
static void put_bits(uint8_t *buf, size_t *bitpos, unsigned val, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        if ((val >> i) & 1u)
            buf[*bitpos >> 3] |= (uint8_t)(1u << (*bitpos & 7));
        (*bitpos)++;
    }
}

/* Encode `n` literal bytes as a 9-bit LSB-first LZW stream (skip word + each
 * byte as a literal code + end code 256). Matches the decoder's reader; valid
 * because the dictionary never reaches 512 here, so the width stays 9. Returns
 * the byte length. */
static size_t lzw_literals(const uint8_t *px, unsigned n, uint8_t *out)
{
    size_t bp = 0;
    unsigned i;
    put_bits(out, &bp, 0, 9);          /* skipped first 9 bits */
    for (i = 0; i < n; i++)
        put_bits(out, &bp, px[i], 9);
    put_bits(out, &bp, 256, 9);        /* end */
    return (bp + 7) / 8;
}

/* Build a one-or-two-frame mode-28 IotaFilm (ACEF + PALE). `technique` 1=LZW,
 * 2=raw; `flags` bit0 = delta. `frames` is nframes * FRM bytes. Returns malloc'd
 * buffer, *len set. */
static uint8_t *build_film(unsigned technique, unsigned flags,
                           const uint8_t *frames, unsigned nframes, size_t *len)
{
    uint8_t blocks[512];
    size_t bl = 0;
    unsigned f;
    for (f = 0; f < nframes; f++) {
        const uint8_t *px = frames + (size_t)f * FRM;
        uint8_t data[64] = {0};
        size_t dlen;
        uint32_t L;
        if (technique == 2) { memcpy(data, px, FRM); dlen = FRM; }
        else { dlen = lzw_literals(px, FRM, data); }
        L = (uint32_t)(8 + dlen);          /* [len][data][len] */
        put_u32(blocks, bl, L); bl += 4;
        memcpy(blocks + bl, data, dlen); bl += dlen;
        put_u32(blocks, bl, L); bl += 4;
    }
    put_u32(blocks, bl, 0); bl += 4;       /* terminating zero word */

    {
        uint32_t flen = (uint32_t)(64 + bl);          /* film header + frames */
        uint32_t acef_size = 8 + flen;
        uint32_t pale_size = 36 + 256 * 4;
        size_t total = acef_size + pale_size;
        uint8_t *b = calloc(1, total);
        size_t film, pale, i;

        /* ACEF chunk */
        memcpy(b, "ACEF", 4);
        put_u32(b, 4, acef_size);
        film = 8;
        put_u32(b, film + 0, flen);
        b[film + 4] = 'T'; b[film + 5] = 0x0d;        /* name "T" + CR */
        put_u32(b, film + 16, 64);                    /* offset to film start */
        put_u32(b, film + 20, W * 2);                 /* width in OS units */
        put_u32(b, film + 24, H * 2);
        put_u32(b, film + 28, 28);                    /* mode */
        put_u32(b, film + 32, technique);
        put_u32(b, film + 36, flags);
        memcpy(b + film + 64, blocks, bl);

        /* PALE chunk: 9 header words then 256 [idx][R][G][B] words */
        pale = acef_size;
        memcpy(b + pale, "PALE", 4);
        put_u32(b, pale + 4, pale_size);
        put_u32(b, pale + 20, 3);                     /* Log2BPP = 8bpp */
        for (i = 0; i < 256; i++) {
            size_t o = pale + 36 + i * 4;
            b[o + 0] = (uint8_t)i;                    /* index */
            b[o + 1] = (uint8_t)i;                    /* R */
            b[o + 2] = (uint8_t)(2 * i);              /* G */
            b[o + 3] = (uint8_t)(3 * i);              /* B */
        }
        *len = total;
        return b;
    }
}

static void test_one(const char *what, unsigned technique, unsigned flags,
                     const uint8_t *frames, unsigned nframes,
                     const uint8_t *expect /* nframes*FRM running frames */)
{
    size_t len;
    uint8_t *film = build_film(technique, flags, frames, nframes, &len);
    char err[256] = {0};
    ReplayTca *t = replay_tca_open(film, len, err, sizeof err);
    unsigned f;
    if (t == NULL) { fprintf(stderr, "FAIL: %s open: %s\n", what, err); failures++; free(film); return; }
    CHECK(replay_tca_width(t) == W && replay_tca_height(t) == H, "dimensions");
    CHECK(replay_tca_frame_count(t) == nframes, "frame count");
    {
        const uint8_t *pal = replay_tca_palette(t);
        CHECK(pal[10*3] == 10 && pal[10*3+1] == 20 && pal[10*3+2] == 30, "palette entry");
    }
    for (f = 0; f < nframes; f++) {
        uint8_t out[FRM];
        int r = replay_tca_next_frame(t, out, err, sizeof err);
        char m[64];
        snprintf(m, sizeof m, "%s frame %u decode", what, f);
        CHECK(r == 1, m);
        if (r == 1) {
            snprintf(m, sizeof m, "%s frame %u pixels", what, f);
            CHECK(memcmp(out, expect + (size_t)f * FRM, FRM) == 0, m);
        }
    }
    CHECK(replay_tca_next_frame(t, (uint8_t[FRM]){0}, err, sizeof err) == 0, "end of film");
    replay_tca_close(t);
    free(film);
}

int main(void)
{
    /* raw single frame */
    const uint8_t raw[FRM] = { 3, 1, 2, 0 };
    test_one("raw", 2, 0, raw, 1, raw);

    /* LZW single frame (literals) */
    const uint8_t lz[FRM] = { 10, 20, 30, 40 };
    test_one("lzw", 1, 0, lz, 1, lz);

    /* Delta: two raw frames; the running frame is A then A^B */
    {
        uint8_t frames[2 * FRM] = { 11,22,33,44,  1,2,3,4 };
        uint8_t expect[2 * FRM];
        unsigned i;
        memcpy(expect, frames, FRM);
        for (i = 0; i < FRM; i++) expect[FRM + i] = frames[i] ^ frames[FRM + i];
        test_one("delta", 2, 1 /*delta*/, frames, 2, expect);
    }

    if (failures == 0)
        printf("OK\n");
    return failures != 0;
}
