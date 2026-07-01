/*
 * replay_escape130.c -- Eidos/Acorn "Escape 2.0" (Replay video format 130).
 * See include/replay/replay_escape130.h.
 *
 * The BITSTREAM DECODER here (everything that turns an entropy-coded chunk into
 * the persistent per-block state) is a clean-room implementation written strictly
 * from the behavioural specification (docs/spec/eidos-escape.md § 130); no existing
 * decoder was consulted. The RENDER (block state -> RGB565) lives in
 * dec130_render.c, a hand-written reimplementation of DEC130.DLL's display path
 * (reverse-engineered from the DLL, bit-exact to it). This file builds each
 * block's "expanded" colour word from the DLL colour tables, hands the block
 * buffer to dec130_render(), and expands its RGB565 to RGB888.
 */
#include "replay/replay_escape130.h"

#include <stdlib.h>
#include <string.h>

#include "dec130_render.h"        /* dec130_render(): the RE'd RGB565 render     */
#include "dec130_render_tbl.h"    /* DEC130.DLL colour tables R_CR / R_CB / R_LUM */

/* ---- bit reader (spec § 3): LSB-first -------------------------------------- */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bytepos;
    int bitpos;    /* 0..7 */
    int eof;       /* set once a read goes past the end */
} BitReader;

static int rd_bit(BitReader *b)
{
    int bit;
    if (b->bytepos >= b->len) { b->eof = 1; return 0; }
    bit = (b->data[b->bytepos] >> b->bitpos) & 1;
    if (++b->bitpos == 8) { b->bitpos = 0; b->bytepos++; }
    return bit;
}

/* Assemble n bits; the first bit read is the least significant. */
static unsigned rd_bits(BitReader *b, int n)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++)
        v |= (unsigned)rd_bit(b) << i;
    return v;
}

/* ---- constant tables (spec § 6/8) ----------------------------------------- */
static const int Y_DIFF[8] = { -4, -3, -2, -1, 1, 2, 3, 4 };

static const uint32_t C_DIFF[8] = {
    0x00010000u, 0x01010000u, 0x01000000u, 0x00FF0000u,
    0xFFFF0000u, 0xFEFF0000u, 0xFF000000u, 0xFF010000u
};

/* The four signs (TL,TR,BL,BR) for each 6-bit sidx (spec § 8). */
static const int8_t SIGN_TUPLES[64][4] = {
    { 0, 0, 0, 0}, {-1,+1, 0, 0}, {+1,-1, 0, 0}, {-1, 0,+1, 0},
    {-1,+1,+1, 0}, { 0,-1,+1, 0}, {+1,-1,+1, 0}, {-1,-1,+1, 0},
    {+1, 0,-1, 0}, { 0,+1,-1, 0}, {+1,+1,-1, 0}, {-1,+1,-1, 0},
    {+1,-1,-1, 0}, {-1, 0, 0,+1}, {-1,+1, 0,+1}, { 0,-1, 0,+1},
    { 0, 0, 0, 0}, {+1,-1, 0,+1}, {-1,-1, 0,+1}, {-1, 0,+1,+1},
    {-1,+1,+1,+1}, { 0,-1,+1,+1}, {+1,-1,+1,+1}, {-1,-1,+1,+1},
    { 0, 0,-1,+1}, {+1, 0,-1,+1}, {-1, 0,-1,+1}, { 0,+1,-1,+1},
    {+1,+1,-1,+1}, {-1,+1,-1,+1}, { 0,-1,-1,+1}, {+1,-1,-1,+1},
    { 0, 0, 0, 0}, {-1,-1,-1,+1}, {+1, 0, 0,-1}, { 0,+1, 0,-1},
    {+1,+1, 0,-1}, {-1,+1, 0,-1}, {+1,-1, 0,-1}, { 0, 0,+1,-1},
    {+1, 0,+1,-1}, {-1, 0,+1,-1}, { 0,+1,+1,-1}, {+1,+1,+1,-1},
    {-1,+1,+1,-1}, { 0,-1,+1,-1}, {+1,-1,+1,-1}, {-1,-1,+1,-1},
    { 0, 0, 0, 0}, {+1, 0,-1,-1}, { 0,+1,-1,-1}, {+1,+1,-1,-1},
    {-1,+1,-1,-1}, {+1,-1,-1,-1}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}
};

struct ReplayEsc130 {
    int w, h, bw, bh;       /* frame + block grid (bw=w/2, bh=h/2) */
    size_t total;           /* bw*bh blocks */
    uint32_t *blk;          /* persistent block-state words (word0) */
    uint8_t *tex;           /* per-block "textured" flag */
    uint32_t sign_tbl[64];  /* SIGN_TUPLES packed into word bits 8..15 */
    Dec130Block *rbuf;      /* render scratch: total+1 blocks (rbuf[0] = seed) */
    uint16_t *out565;       /* render scratch: w*h RGB565 */
};

/* ---- skip run-length VLC (spec § 5) --------------------------------------- */
static unsigned read_skip(BitReader *b)
{
    unsigned v;
    if (rd_bit(b) == 1) return 0;
    v = rd_bits(b, 3);  if (v != 0) return v;
    v = rd_bits(b, 8);  if (v != 0) return v + 7;
    v = rd_bits(b, 15); return v + 262;
}

/* ---- per-block decode (spec § 6): luma mode then chroma mode --------------- */
static uint32_t decode_block(ReplayEsc130 *s, BitReader *b, uint32_t pred, int *out_tex)
{
    uint32_t lbits, cbits;
    int luma_copy = 0, luma_signs = 0, chroma_copy = 0;

    /* Luma prefix: 1 -> SIGNS; 00 -> COPY; 010 -> DELTA; 011 -> ABS. */
    if (rd_bit(b) == 1) {
        unsigned sidx = rd_bits(b, 6);
        unsigned step = rd_bits(b, 2);
        unsigned ya   = rd_bits(b, 5);
        luma_signs = 1;
        lbits = s->sign_tbl[sidx] | (step << 6) | (ya << 1);
    } else if (rd_bit(b) == 0) {
        luma_copy = 1;
        lbits = pred & 0x3Fu;
    } else if (rd_bit(b) == 0) {
        /* DELTA: plain integer add, not masked to 6 bits (spec § 6.1). */
        unsigned d = rd_bits(b, 3);
        int yv = (int)(pred & 0x3Fu) + Y_DIFF[d];
        lbits = (uint32_t)yv;
    } else {
        lbits = rd_bits(b, 6);
    }

    /* Chroma prefix: 0 -> COPY; 10 -> DELTA; 11 -> ABS. */
    if (rd_bit(b) == 0) {
        chroma_copy = 1;
        cbits = pred & 0xFFFF0000u;
    } else if (rd_bit(b) == 0) {
        unsigned d = rd_bits(b, 3);
        cbits = (pred & 0xFFFF0000u) + C_DIFF[d];   /* packed add (spec § 7.2) */
    } else {
        unsigned cb = rd_bits(b, 5);
        unsigned cr = rd_bits(b, 5);
        cbits = (cb << 16) | (cr << 24);
    }

    /* § 6.3: full copy preserves the predictor's texture bits but renders flat;
     * a block is "textured" iff *this* decode chose the SIGNS luma mode. */
    if (luma_copy && chroma_copy) {
        *out_tex = 0;
        return pred;
    }
    *out_tex = luma_signs;
    return lbits | cbits;
}

/* ---- render glue (spec § 9.1): the DLL "expanded" colour word -------------- */
/* Per-lane saturating clamp for a flat block's expanded word (DEC130.DLL
 * 0x1000cde8): underflow clears a lane to 0, overflow saturates it. */
static uint32_t clamp_build(uint32_t c)
{
    if (c & 0x10020100u) {
        if ((c & 0x10000000u) && !(c & 0x20000000u)) c &= 0xe00fffffu;  /* red   */
        if ((c & 0x100u)      && !(c & 0x200u))      c &= 0xfffffe00u;  /* green */
        if ((c & 0x20000u)    && !(c & 0x40000u))    c &= 0xfffc03ffu;  /* blue  */
    }
    {
        uint32_t e = c + 0x600c03u;
        if (e & 0x10020100u) {
            if (e & 0x10000000u) c = (c & 0xf00fffffu) | 0x0f900000u;   /* red   */
            if (e & 0x100u)      c = (c & 0xffffff00u) | 0x000000fcu;   /* green */
            if (e & 0x20000u)    c = (c & 0xfffe03ffu) | 0x0001f000u;   /* blue  */
        }
    }
    return c & 0xcff9fcffu;
}

/* Expanded colour word ("word4") for block bi, from the DLL colour tables. */
static uint32_t block_base(const ReplayEsc130 *s, size_t bi)
{
    uint32_t w = s->blk[bi];
    unsigned yavg = w & 0x3Fu;
    unsigned cb = (w >> 16) & 0xFFu;
    unsigned cr = (w >> 24) & 0xFFu;
    uint32_t flags = 0x80000000u | (s->tex[bi] ? 0x40000000u : 0u);
    uint32_t sum = R_CR[cr & 63] + R_CB[cb] + (R_LUM[yavg] | flags);
    return (sum & 0x40000000u) ? sum : clamp_build(sum);
}

/* ---- public API ----------------------------------------------------------- */
ReplayEsc130 *replay_esc130_open(unsigned width, unsigned height)
{
    ReplayEsc130 *s;
    int si;
    if (width == 0 || height == 0 || (width % 2) || (height % 2))
        return NULL;
    s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->w = (int)width; s->h = (int)height;
    s->bw = (int)(width / 2); s->bh = (int)(height / 2);
    s->total = (size_t)s->bw * (size_t)s->bh;
    s->blk = calloc(s->total ? s->total : 1, sizeof(uint32_t));
    s->tex = calloc(s->total ? s->total : 1, 1);
    s->rbuf = calloc(s->total + 1, sizeof(Dec130Block));
    s->out565 = calloc((size_t)s->w * (size_t)s->h, sizeof(uint16_t));
    if (s->blk == NULL || s->tex == NULL || s->rbuf == NULL || s->out565 == NULL) {
        replay_esc130_close(s);
        return NULL;
    }
    /* Build the packed texture sign table (spec § 8): code(+1)=1, code(-1)=2. */
    for (si = 0; si < 64; si++) {
        uint32_t entry = 0;
        int k;
        for (k = 0; k < 4; k++) {
            int sgn = SIGN_TUPLES[si][k];
            unsigned code = (sgn > 0) ? 1u : (sgn < 0) ? 2u : 0u;
            entry |= code << (8 + 2 * k);
        }
        s->sign_tbl[si] = entry;
    }
    return s;
}

void replay_esc130_close(ReplayEsc130 *s)
{
    if (s == NULL)
        return;
    free(s->blk);
    free(s->tex);
    free(s->rbuf);
    free(s->out565);
    free(s);
}

int replay_esc130_decode(ReplayEsc130 *s, const uint8_t *chunk, size_t clen)
{
    BitReader b;
    size_t i;
    if (s == NULL || chunk == NULL)
        return -1;
    if (clen < 16)                 /* no bitstream: frame unchanged (spec § 1) */
        return 0;

    b.data = chunk + 16; b.len = clen - 16;
    b.bytepos = 0; b.bitpos = 0; b.eof = 0;

    i = read_skip(&b);             /* initial skip before the first coded block */
    while (i < s->total) {
        uint32_t pred;
        int tex = 0;
        if (b.eof) break;          /* truncated stream: stop, keep persistence */
        pred = (i > 0) ? s->blk[i - 1] : 0u;  /* block 0 predictor = all-zero seed */
        s->blk[i] = decode_block(s, &b, pred, &tex);
        s->tex[i] = (uint8_t)tex;
        i += 1 + read_skip(&b);
    }
    return 0;
}

void replay_esc130_render(ReplayEsc130 *s, uint8_t *rgb)
{
    size_t i;
    int oy, ox;

    /* rbuf[0] = seed/sentinel; block i at rbuf[i+1]. */
    s->rbuf[0].word0 = 0; s->rbuf[0].word4 = 0;
    for (i = 0; i < s->total; i++) {
        s->rbuf[i + 1].word0 = s->blk[i];
        s->rbuf[i + 1].word4 = block_base(s, i);
    }
    dec130_render(s->rbuf, s->w, s->h, s->out565);

    for (oy = 0; oy < s->h; oy++)
        for (ox = 0; ox < s->w; ox++) {
            size_t pi = (size_t)oy * (size_t)s->w + (size_t)ox;
            uint16_t c = s->out565[pi];
            unsigned r5 = (c >> 11) & 0x1Fu, g6 = (c >> 5) & 0x3Fu, b5 = c & 0x1Fu;
            size_t off = pi * 3;
            rgb[off + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            rgb[off + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            rgb[off + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
}
