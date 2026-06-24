#include "replay/replay_tca.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LZW_DICT_SIZE 65536u

struct ReplayTca {
    const uint8_t *data;
    size_t len;
    unsigned width, height;
    unsigned mode, technique, update; /* update = delta flag */
    uint8_t palette[256 * 3];
    size_t film_start;   /* abs offset of the ACE film data (ACEF + 8) */
    size_t film_end;     /* film_start + film length */
    size_t cursor;       /* abs offset of the next frame block */
    unsigned count;
    size_t frame_words;  /* decoded screen size in bytes for the mode */
    uint8_t *frame;      /* running screen (frame_words) */
    uint8_t *scratch;    /* delta decode buffer (frame_words) */
    uint8_t *dsym;       /* LZW dict: terminal symbols */
    uint16_t *dprev;     /* LZW dict: backlinks */
};

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err != NULL && errlen != 0)
        snprintf(err, errlen, "%s", msg);
}

static uint32_t rd_u32(const uint8_t *d, size_t o)
{
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8)
         | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

/* Walk the IotaFilm chunk list (each [id:4][size incl. header:u32le]) for `tag`.
 * Returns the chunk offset or (size_t)-1. */
static size_t find_chunk(const uint8_t *d, size_t len, const char *tag)
{
    size_t p = 0;
    while (p + 8 <= len) {
        if (memcmp(d + p, tag, 4) == 0)
            return p;
        uint32_t sz = rd_u32(d, p + 4);
        if (sz < 8 || sz > len - p)
            break;
        p += sz;
    }
    return (size_t)-1;
}

/* Euclid LZW (technique 1): LSB-first bits, dictionary reset per frame, skip the
 * first 9 bits, 9..16-bit codes, code 256 ends, dictionary entries from 257.
 * Decodes exactly `dlen` bytes into `dst`. Returns 0 on success, -1 on error. */
static int lzw_decode(ReplayTca *t, const uint8_t *src, size_t slen,
                      uint8_t *dst, size_t dlen)
{
    size_t bitpos = 0;
    size_t total_bits = slen * 8;
    unsigned dict_pos = 257, dict_lim = 1u << 9, idx_bits = 9;
    size_t pos = 0;
    long lastidx = -1;
    unsigned skipped = 0;

    /* read `nb` bits LSB-first; returns -1 on underrun via the out param flag */
#define RD(nb, out) do {                                            \
        unsigned _n = (nb), _v = 0, _g = 0;                         \
        while (_g < _n) {                                           \
            if (bitpos >= total_bits) return -1;                    \
            unsigned _bit = (src[bitpos >> 3] >> (bitpos & 7)) & 1u;\
            _v |= _bit << _g; _g++; bitpos++;                       \
        }                                                           \
        (out) = _v;                                                 \
    } while (0)

    RD(9, skipped);
    (void)skipped;
    for (;;) {
        unsigned idx;
        RD(idx_bits, idx);
        if (idx == 256)
            break;
        if (idx < dict_pos) {
            unsigned tot = 1, tcur = idx;
            size_t end;
            while (tcur > 256) { tcur = t->dprev[tcur]; tot++; }
            if (pos + tot > dlen) return -1;
            end = pos + tot - 1;
            tcur = idx;
            while (tcur > 256) { dst[end--] = t->dsym[tcur]; tcur = t->dprev[tcur]; }
            dst[end] = (uint8_t)tcur;
            if (lastidx >= 0 && dict_pos < dict_lim) {
                t->dsym[dict_pos] = dst[pos];
                t->dprev[dict_pos] = (uint16_t)lastidx;
                dict_pos++;
            }
            pos += tot;
        } else if (idx == dict_pos) {
            unsigned tot = 1, tcur;
            size_t end;
            uint8_t lastsym;
            if (lastidx < 0) return -1;
            tcur = (unsigned)lastidx;
            while (tcur > 256) { tcur = t->dprev[tcur]; tot++; }
            if (pos + tot > dlen) return -1;
            end = pos + tot - 1;
            tcur = (unsigned)lastidx;
            while (tcur > 256) { dst[end--] = t->dsym[tcur]; tcur = t->dprev[tcur]; }
            dst[end] = (uint8_t)tcur;
            lastsym = dst[pos];
            pos += tot;
            if (pos >= dlen) return -1;
            dst[pos++] = lastsym;
            if (dict_pos < dict_lim) {
                t->dsym[dict_pos] = lastsym;
                t->dprev[dict_pos] = (uint16_t)lastidx;
                dict_pos++;
            }
        } else {
            return -1;
        }
        lastidx = (long)idx;
        if (dict_pos == dict_lim && idx_bits < 16) {
            dict_lim <<= 1;
            idx_bits++;
        }
    }
#undef RD
    return pos == dlen ? 0 : -1;
}

/* Euclid RLE (technique 0). */
static int rle_decode(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen)
{
    size_t sp = 0, pos = 0;
    while (pos < dlen) {
        size_t run;
        uint8_t pix;
        if (sp >= slen) return -1;
        run = src[sp++];
        if (run == 0) {
            if (sp >= slen) return -1;
            run = src[sp++];
            if (run == 0) {
                if (sp + 3 > slen) return -1;
                run = ((size_t)src[sp] << 16) | ((size_t)src[sp + 1] << 8) | src[sp + 2];
                sp += 3;
            } else {
                if (sp >= slen) return -1;
                run = (run << 8) | src[sp++];
            }
        }
        if ((run & 1) == 0) {
            if (sp >= slen) return -1;
            pix = src[sp++];
        } else {
            pix = 0;
        }
        run >>= 1;
        if (pos + run > dlen) return -1;
        memset(dst + pos, pix, run);
        pos += run;
    }
    return 0;
}

ReplayTca *replay_tca_open(const uint8_t *data, size_t len,
                           char *err, size_t errlen)
{
    ReplayTca *t;
    size_t acef, film, pale;
    uint32_t flen, foff;
    size_t p;
    size_t i;

    acef = find_chunk(data, len, "ACEF");
    if (acef == (size_t)-1) { set_err(err, errlen, "no ACEF chunk"); return NULL; }
    film = acef + 8;
    if (film + 40 > len) { set_err(err, errlen, "truncated ACE film header"); return NULL; }

    flen = rd_u32(data, film + 0);
    foff = rd_u32(data, film + 16);
    if (flen < foff || film + flen > len || foff < 40) {
        set_err(err, errlen, "bad ACE film header"); return NULL;
    }

    t = calloc(1, sizeof *t);
    if (t == NULL) { set_err(err, errlen, "out of memory"); return NULL; }
    t->data = data;
    t->len = len;
    t->width  = rd_u32(data, film + 20) / 2;  /* OS units -> pixels (2 per px) */
    t->height = rd_u32(data, film + 24) / 2;
    t->mode = rd_u32(data, film + 28);
    t->technique = rd_u32(data, film + 32);
    t->update = rd_u32(data, film + 36) & 1u;
    t->film_start = film;
    t->film_end = film + flen;

    if (t->technique > 2) {
        set_err(err, errlen, "unknown TCA compression technique"); free(t); return NULL;
    }
    if (t->width == 0 || t->height == 0) {
        set_err(err, errlen, "zero TCA dimensions"); free(t); return NULL;
    }
    /* Decompressed (packed) screen size by RISC OS mode. The display is always
     * width*height 8-bit indices; output expansion (nibble unpack / vertical
     * doubling) happens in replay_tca_next_frame. */
    switch (t->mode) {
    case 21: case 28:                     /* 8-bit direct */
        t->frame_words = (size_t)t->width * t->height; break;
    case 27:                              /* 4-bit, full height */
        if (t->width & 1u) goto odd;
        t->frame_words = (size_t)(t->width / 2) * t->height; break;
    case 12: case 13: case 39:            /* 4-bit, half height (doubled) */
        if ((t->width & 1u) || (t->height & 1u)) goto odd;
        t->frame_words = (size_t)(t->width / 2) * (t->height / 2); break;
    case 15: case 36: case 40:            /* 8-bit, half height (doubled) */
        if (t->height & 1u) goto odd;
        t->frame_words = (size_t)t->width * (t->height / 2); break;
    default: {
        char m[64];
        snprintf(m, sizeof m, "TCA screen mode %u not supported", t->mode);
        set_err(err, errlen, m); free(t); return NULL;
    }
    odd:
        set_err(err, errlen, "TCA dimensions not even for this mode");
        free(t); return NULL;
    }

    /* PALE: 9 header words then the ColourTrans palette, on disk [idx][R][G][B].
     * The entry count comes from the chunk size (4-bit films carry 16). */
    pale = find_chunk(data, len, "PALE");
    if (pale != (size_t)-1) {
        uint32_t psize = rd_u32(data, pale + 4);
        size_t nent = psize > 36 ? (psize - 36) / 4 : 0;
        if (nent > 256) nent = 256;
        if (pale + 36 + nent * 4 <= len) {
            for (i = 0; i < nent; i++) {
                size_t o = pale + 36 + (size_t)i * 4;
                t->palette[i * 3 + 0] = data[o + 1];
                t->palette[i * 3 + 1] = data[o + 2];
                t->palette[i * 3 + 2] = data[o + 3];
            }
        }
    } /* else: black palette (a greyscale ramp would also be reasonable) */

    /* Count frames (blocks [len][data][len], advance by len, 0 terminates). */
    p = film + foff;
    t->count = 0;
    while (p + 4 <= t->film_end) {
        uint32_t L = rd_u32(data, p);
        if (L == 0) break;
        if (L < 8 || L > t->film_end - p) break;
        t->count++;
        p += L;
    }

    t->frame = calloc(1, t->frame_words ? t->frame_words : 1);
    t->scratch = calloc(1, t->frame_words ? t->frame_words : 1);
    t->dsym = malloc(LZW_DICT_SIZE);
    t->dprev = malloc(LZW_DICT_SIZE * sizeof(uint16_t));
    if (!t->frame || !t->scratch || !t->dsym || !t->dprev) {
        set_err(err, errlen, "out of memory");
        replay_tca_close(t);
        return NULL;
    }
    t->cursor = film + foff;
    return t;
}

void replay_tca_close(ReplayTca *t)
{
    if (t == NULL) return;
    free(t->frame);
    free(t->scratch);
    free(t->dsym);
    free(t->dprev);
    free(t);
}

unsigned replay_tca_width(const ReplayTca *t) { return t->width; }
unsigned replay_tca_height(const ReplayTca *t) { return t->height; }
unsigned replay_tca_frame_count(const ReplayTca *t) { return t->count; }
const uint8_t *replay_tca_palette(const ReplayTca *t) { return t->palette; }

/* Expand the decoded (packed) screen `t->frame` to `out` (width*height 8-bit
 * indices), per the RISC OS screen mode: nibble unpack for 4-bit modes (low
 * nibble = left pixel) and vertical doubling for the half-height modes. */
static void tca_expand(const ReplayTca *t, uint8_t *out)
{
    unsigned W = t->width, H = t->height, r, c;
    const uint8_t *f = t->frame;
    switch (t->mode) {
    case 27:
        for (r = 0; r < H; r++) {
            const uint8_t *s = f + (size_t)r * (W / 2);
            uint8_t *d = out + (size_t)r * W;
            for (c = 0; c < W / 2; c++) {
                d[2 * c]     = (uint8_t)(s[c] & 0x0F);
                d[2 * c + 1] = (uint8_t)(s[c] >> 4);
            }
        }
        break;
    case 12: case 13: case 39:
        for (r = 0; r < H / 2; r++) {
            const uint8_t *s = f + (size_t)r * (W / 2);
            uint8_t *d0 = out + (size_t)(2 * r) * W;
            uint8_t *d1 = d0 + W;
            for (c = 0; c < W / 2; c++) {
                d0[2 * c]     = (uint8_t)(s[c] & 0x0F);
                d0[2 * c + 1] = (uint8_t)(s[c] >> 4);
            }
            memcpy(d1, d0, W);
        }
        break;
    case 15: case 36: case 40:
        for (r = 0; r < H / 2; r++) {
            const uint8_t *s = f + (size_t)r * W;
            uint8_t *d0 = out + (size_t)(2 * r) * W;
            memcpy(d0, s, W);
            memcpy(d0 + W, s, W);
        }
        break;
    default: /* 21 / 28: 8-bit direct */
        memcpy(out, f, (size_t)W * H);
        break;
    }
}

int replay_tca_next_frame(ReplayTca *t, uint8_t *out, char *err, size_t errlen)
{
    uint32_t L;
    const uint8_t *pkt;
    size_t plen;
    uint8_t *dst;
    int rc;

    if (t->cursor + 4 > t->film_end)
        return 0;
    L = rd_u32(t->data, t->cursor);
    if (L == 0)
        return 0;
    if (L < 8 || L > t->film_end - t->cursor) {
        set_err(err, errlen, "bad TCA frame block length");
        return -1;
    }
    pkt = t->data + t->cursor + 4;   /* data after the leading length word */
    plen = L - 4;                    /* includes the trailing length word; ignored */

    dst = t->update ? t->scratch : t->frame;
    switch (t->technique) {
    case 1: rc = lzw_decode(t, pkt, plen, dst, t->frame_words); break;
    case 0: rc = rle_decode(pkt, plen, dst, t->frame_words); break;
    default: /* 2 = raw */
        if (plen < t->frame_words) { rc = -1; }
        else { memcpy(dst, pkt, t->frame_words); rc = 0; }
        break;
    }
    if (rc != 0) {
        set_err(err, errlen, "TCA frame decode failed");
        return -1;
    }
    if (t->update) {
        size_t i;
        for (i = 0; i < t->frame_words; i++)
            t->frame[i] ^= t->scratch[i];
    }
    tca_expand(t, out);
    t->cursor += L;
    return 1;
}
