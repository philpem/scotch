/* Unit test for the TCA/ACEF decoder. Builds tiny synthetic IotaFilms (a 2x2,
 * mode-28 film: ACEF + PALE) exercising the raw, LZW and Delta paths, and checks
 * the decoded palette indices and the parsed palette. The format and the LZW
 * coding are re-derived here independently of replay_tca.c. */

#include "replay/replay_tca.h"
#include "replay/replay_sound.h"

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
static uint8_t *build_film(unsigned mode, unsigned technique, unsigned flags,
                           const uint8_t *frames, size_t frame_bytes,
                           unsigned nframes, size_t *len)
{
    uint8_t blocks[512];
    size_t bl = 0;
    unsigned f;
    for (f = 0; f < nframes; f++) {
        const uint8_t *px = frames + (size_t)f * frame_bytes;
        uint8_t data[64] = {0};
        size_t dlen;
        uint32_t L;
        if (technique == 2) { memcpy(data, px, frame_bytes); dlen = frame_bytes; }
        else { dlen = lzw_literals(px, (unsigned)frame_bytes, data); }
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
        put_u32(b, film + 28, mode);                  /* mode */
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

static void test_one(const char *what, unsigned mode, unsigned technique,
                     unsigned flags, const uint8_t *frames, size_t frame_bytes,
                     unsigned nframes,
                     const uint8_t *expect /* nframes*FRM display frames */)
{
    size_t len;
    uint8_t *film = build_film(mode, technique, flags, frames, frame_bytes,
                               nframes, &len);
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

/* Build a minimal SOUN chunk (just the WAVn sample data the decoder reads) and
 * check replay_tca_decode_audio. tag = "WAV1" or "WAV2"; `snd`/`nbytes` is the
 * sample data placed at SOUN+36 (= inner+28, after tag+size+5 words). */
static void test_audio(const char *tag, const uint8_t *snd, size_t nbytes,
                       size_t expect_count)
{
    uint8_t buf[256] = {0};
    uint32_t wavsize = (uint32_t)(28 + nbytes);
    size_t total = 8 + wavsize;
    char err[256] = {0};
    size_t got = 0;
    int16_t *pcm;
    memcpy(buf, "SOUN", 4);
    put_u32(buf, 4, (uint32_t)total);
    memcpy(buf + 8, tag, 4);
    put_u32(buf, 12, wavsize);          /* WAV chunk size */
    memcpy(buf + 36, snd, nbytes);      /* sample data at inner(8)+28 */
    pcm = replay_tca_decode_audio(buf, total, &got, err, sizeof err);
    if (pcm == NULL) { fprintf(stderr, "FAIL: %s audio: %s\n", tag, err); failures++; return; }
    CHECK(got == expect_count, "audio sample count");
    if (strcmp(tag, "WAV1") == 0) {
        size_t i;
        for (i = 0; i < nbytes && i < got; i++)
            CHECK(pcm[i] == replay_sound_vidc_e8_to_s16(snd[i]), "WAV1 sample");
    }
    free(pcm);
}

int main(void)
{
    /* raw single frame (mode 28, 8-bit) */
    const uint8_t raw[FRM] = { 3, 1, 2, 0 };
    test_one("raw", 28, 2, 0, raw, FRM, 1, raw);

    /* LZW single frame (literals) */
    const uint8_t lz[FRM] = { 10, 20, 30, 40 };
    test_one("lzw", 28, 1, 0, lz, FRM, 1, lz);

    /* Delta: two raw frames; the running frame is A then A^B */
    {
        uint8_t frames[2 * FRM] = { 11,22,33,44,  1,2,3,4 };
        uint8_t expect[2 * FRM];
        unsigned i;
        memcpy(expect, frames, FRM);
        for (i = 0; i < FRM; i++) expect[FRM + i] = frames[i] ^ frames[FRM + i];
        test_one("delta", 28, 2, 1 /*delta*/, frames, FRM, 2, expect);
    }

    /* Mode 27 (4-bit, full height): 2 packed bytes -> 2 rows of 2 px each, low
     * nibble = left pixel. [0x21,0x43] -> [1,2] then [3,4]. */
    {
        const uint8_t packed[2] = { 0x21, 0x43 };
        const uint8_t expect[FRM] = { 1, 2, 3, 4 };
        test_one("mode27", 27, 2, 0, packed, 2 /*frame_bytes*/, 1, expect);
    }

    /* Audio: WAV1 (8-bit VIDC log) -> one sample per byte; WAV2 (4-bit ADPCM)
     * -> two samples per byte. */
    {
        const uint8_t w1[4] = { 0x00, 0x80, 0x40, 0xC0 };
        const uint8_t w2[3] = { 0x12, 0x34, 0x56 };
        test_audio("WAV1", w1, sizeof w1, sizeof w1);
        test_audio("WAV2", w2, sizeof w2, sizeof w2 * 2);
    }

    if (failures == 0)
        printf("OK\n");
    return failures != 0;
}
