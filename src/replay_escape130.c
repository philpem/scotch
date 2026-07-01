/*
 * replay_escape130.c -- Eidos/Acorn "Escape 2.0" (Replay video format 130).
 * See include/replay/replay_escape130.h.
 *
 * The whole codec in one file, but with two distinct provenances:
 *
 *  - the BITSTREAM DECODER (everything that turns an entropy-coded chunk into the
 *    persistent per-block state) is a clean-room implementation written strictly
 *    from the behavioural specification (docs/spec/eidos-escape.md § 130); no
 *    existing decoder was consulted;
 *  - the RENDER (block state -> RGB565) is a hand-written reimplementation of
 *    DEC130.DLL's display path, reverse-engineered from the DLL (by probing its
 *    output as an oracle) and bit-exact to it on every test movie.
 *
 * The codec stores a low-resolution grid of 2x2-pixel blocks; each block carries a
 * base luma, a chroma pair, and an optional 2-bit-per-sub-pixel texture pattern,
 * all delta-coded and persistent across frames. The display 2x upsamples that grid
 * to the full frame with a separable blend, rendering textured blocks sharply.
 */
#include "replay/replay_escape130.h"

#include <stdlib.h>
#include <string.h>

#include "escape130_tables.h"

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

/*
 * One decoded 2x2-pixel block (DEC130.DLL's ctx[2] layout):
 *   word0 : packed luma/chroma plus four 2-bit per-sub-pixel "texture" signs;
 *   word4 : the "expanded" colour -- three channels in disjoint lanes (green @
 *           bits 0..8, blue @ 10..18 half-scaled, red @ 20..28) -- plus a
 *           "textured" flag at bit 30.
 */
typedef struct { uint32_t word0, word4; } Block;

struct ReplayEsc130 {
    int w, h, bw, bh;       /* frame + block grid (bw=w/2, bh=h/2) */
    size_t total;           /* bw*bh blocks */
    uint32_t *blk;          /* persistent block-state words (word0) */
    uint8_t *tex;           /* per-block "textured" flag */
    uint32_t sign_tbl[64];  /* SIGN_TUPLES packed into word bits 8..15 */
    Block *rbuf;            /* render scratch: total+1 blocks (rbuf[0] = seed) */
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
        int yv = (int)(pred & 0x3Fu) + ESC130_Y_DIFF[d];
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
        cbits = (pred & 0xFFFF0000u) + ESC130_C_DIFF[d];   /* packed add (spec § 7.2) */
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

/* ==========================================================================
 * RENDER (DEC130.DLL display path, reverse-engineered; bit-exact to the DLL).
 *
 * Colour arithmetic is SWAR: the three channels live in disjoint lanes of one
 * 32-bit word -- green @ bits 0..8, blue @ 10..18 (half-scaled), red @ 20..28 --
 * each with a guard bit just above its value, so a lane can be added/subtracted
 * and clamped independently of the others.
 * ========================================================================== */
#define COLOUR_MASK   0x3fffffffu    /* the three colour lanes (drops the flags) */
#define TEXTURED_FLAG 0x40000000u    /* word4 bit 30: block carries fresh texture */

/* DEC130.DLL 565 colour tables, dumped from live BSS (built at DLL init):
 * CR=cr@0x18160, LUM=luma@0x18260, CB=cb@0x184e0. Used to build each block's
 * expanded colour word ("word4"). */
static const uint32_t R_CR[64]={0x692014du,0x7420147u,0x7f20142u,0x8a2013cu,0x9520136u,0xa120131u,0xac2012bu,0xb720125u,0xc22011fu,0xce2011au,0xd920114u,0xe120110u,0xea2010bu,0xef20109u,0xf520106u,0xfa20103u,0x10020100u,0x106200fdu,0x10b200fau,0x111200f7u,0x116200f5u,0x11f200f0u,0x127200ecu,0x132200e6u,0x13e200e1u,0x149200dbu,0x154200d5u,0x15f200cfu,0x16b200cau,0x176200c4u,0x181200beu,0x18c200b9u,0x0u,0x0u,0x0u,0x0u,0x802008u,0x1004010u,0x280a028u,0x5014050u,0xff7fdff8u,0xfeffbff0u,0xfd7f5fd8u,0xfafebfb0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x801008u,0x1002010u,0x2805028u,0x500a050u,0xff7feff8u,0xfeffdff0u,0xfd7fafd8u,0xfaff5fb0u,0x0u,0x0u,0x0u,0x0u};
static const uint32_t R_LUM[64]={0x0u,0x400804u,0x801008u,0xc0180cu,0x1002010u,0x1402814u,0x1803018u,0x1c0381cu,0x2004020u,0x2404824u,0x2805028u,0x2c0582cu,0x3006030u,0x3406834u,0x3807038u,0x3c0783cu,0x4008040u,0x4408844u,0x4809048u,0x4c0984cu,0x500a050u,0x540a854u,0x580b058u,0x5c0b85cu,0x600c060u,0x640c864u,0x680d068u,0x6c0d86cu,0x700e070u,0x740e874u,0x780f078u,0x7c0f87cu,0x8010080u,0x8410884u,0x8811088u,0x8c1188cu,0x9012090u,0x9412894u,0x9813098u,0x9c1389cu,0xa0140a0u,0xa4148a4u,0xa8150a8u,0xac158acu,0xb0160b0u,0xb4168b4u,0xb8170b8u,0xbc178bcu,0xc0180c0u,0xc4188c4u,0xc8190c8u,0xcc198ccu,0xd01a0d0u,0xd41a8d4u,0xd81b0d8u,0xdc1b8dcu,0xe01c0e0u,0xe41c8e4u,0xe81d0e8u,0xec1d8ecu,0xf01e0f0u,0xf41e8f4u,0xf81f0f8u,0xfc1f8fcu};
static const uint32_t R_CB[256]={0x10008125u,0x10009d22u,0x1000b920u,0x1000d51du,0x1000f11au,0x10011117u,0x10012d15u,0x10014912u,0x1001650fu,0x1001810cu,0x10019d0au,0x1001b108u,0x1001c906u,0x1001d504u,0x1001e503u,0x1001f101u,0x10020100u,0x10020cffu,0x10021cfdu,0x100228fcu,0x100238fau,0x10024cf8u,0x100264f6u,0x100280f4u,0x10029cf1u,0x1002b8eeu,0x1002d4ebu,0x1002f0e9u,0x10030ce6u,0x100328e3u,0x100344e0u,0x100360deu,0x0u,0x600u,0x900u,0x1200u,0x1600u,0x1800u,0x1900u,0x1a00u,0x2100u,0x2400u,0x2500u,0x2600u,0x2900u,0x4200u,0x4600u,0x4800u,0x0u,0x4900u,0x4a00u,0x5200u,0x5600u,0x5800u,0x5900u,0x5a00u,0x6000u,0x6100u,0x6200u,0x6400u,0x6500u,0x6600u,0x6800u,0x6900u,0x0u,0x6a00u,0x8100u,0x8400u,0x8500u,0x8600u,0x8900u,0x9000u,0x9100u,0x9200u,0x9400u,0x9500u,0x9600u,0x9800u,0x9900u,0x9a00u,0x0u,0xa100u,0xa400u,0xa500u,0xa600u,0xa900u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x3f0fe8u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u};

/* Per-stepidx luma texture step, packed in the colour lanes (SWAR add/sub). */
static const uint32_t R_STEP[4] = { 0x00801008u, 0x01002010u, 0x02805028u, 0x0500a050u };

/* Per-sub-pixel-phase rounding bias for the blend (phase = (oy&1)*2 + (ox&1)).
 * Green and blue share a table; red has its own (and a wider final shift). */
static const uint32_t RND_GREEN_BLUE[4] = { 0, 12, 8, 4 };
static const uint32_t RND_RED[4]        = { 24, 0, 8, 16 };

/*
 * Saturating clamp of one packed channel after a texture step (DEC130.DLL
 * 0x10009860). When the channel's `guard` bit is set the value left its range;
 * the `sign` bit just above it says which way -- set = overflow (saturate: OR in
 * `sat`), clear = underflow (clamp to 0: AND with `clear`). Channels occupy
 * disjoint lanes, so the three clamps are independent and order-free.
 */
static uint32_t clamp_channel(uint32_t v, uint32_t guard, uint32_t sign,
                              uint32_t sat, uint32_t clear)
{
    if (v & guard)
        v = (v & sign) ? (v | sat) : (v & clear);
    return v;
}
static uint32_t clamp_step(uint32_t v)
{
    v = clamp_channel(v, 0x10000000u, 0x20000000u, 0x0ff00000u, 0xf00fffffu); /* red   */
    v = clamp_channel(v, 0x00000100u, 0x00000200u, 0x000000ffu, 0xffffff00u); /* green */
    v = clamp_channel(v, 0x00020000u, 0x00040000u, 0x0001fc00u, 0xfffe03ffu); /* blue  */
    return v;
}

/* Pack one expanded colour word (green@0, blue@10, red@20) to RGB565. */
static uint16_t pack565(uint32_t colour)
{
    return (uint16_t)(((colour >> 12) & 0xf81fu) | ((colour & 0xfcu) << 3));
}

/* (2,1,1)/4 blend of a dominant colour with its horizontal + vertical neighbours,
 * with per-phase rounding, packed to RGB565. The three lanes are summed in
 * parallel; the /4 and the lane widths fold into the shifts (green/blue >>4,
 * red >>5). */
static uint16_t blend565(uint32_t dom, uint32_t horiz, uint32_t vert, int ph)
{
    uint32_t sum = 2u * dom + horiz + vert;
    uint32_t g = ((( sum        & 0x3ffu) + RND_GREEN_BLUE[ph]) >> 4) & 0x3fu;
    uint32_t b = ((((sum >> 10) & 0x3ffu) + RND_GREEN_BLUE[ph]) >> 4) & 0x1fu;
    uint32_t r = ((((sum >> 20) & 0x3ffu) + RND_RED[ph])        >> 5) & 0x1fu;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* One sub-pixel of a textured block: word4 +/- the block's luma step, selected by
 * the 2-bit sign at word0 bit (8 + 2*ph): 01 = +, 10 = -, 00 = none. */
static uint32_t textured_pixel(uint32_t word0, uint32_t word4, int ph)
{
    uint32_t step = R_STEP[(word0 >> 6) & 3];
    if      (word0 & (1u << (8 + 2 * ph))) return word4 + step;
    else if (word0 & (1u << (9 + 2 * ph))) return word4 - step;
    return word4;
}

/* Colour a flat neighbour contributes to the blend: its own colour, unless it is
 * textured, in which case the blend reverts to `self`. */
static uint32_t neighbour_colour(const Block *n, uint32_t self)
{
    return (n->word4 & TEXTURED_FLAG) ? self : (n->word4 & COLOUR_MASK);
}

/* Render the block grid (blocks[0] = seed) to W*H RGB565. */
static void render_blocks(const Block *blocks, int W, int H, uint16_t *out565)
{
    int bw = W / 2;
    int oy, ox;
    /* Real block (bx,by) follows the seed at index 0. */
    #define BLOCK(bx, by) (blocks[((long)(by) * bw + (bx)) + 1])

    for (oy = 0; oy < H; oy++) {
        for (ox = 0; ox < W; ox++) {
            int bx = ox >> 1, by = oy >> 1;
            int px = ox & 1, py = oy & 1, ph = py * 2 + px;
            const Block *blk = &BLOCK(bx, by);
            uint32_t colour = blk->word4 & COLOUR_MASK;
            int edge_col = (ox == 0 || ox == W - 1);
            int edge_row = (oy == 0 || oy == H - 1);
            uint16_t c;

            if (edge_col && !edge_row) {
                /* Leftmost / rightmost pixel column: the plain block colour. */
                c = pack565(colour);
            } else if (blk->word4 & TEXTURED_FLAG) {
                /* Textured block: sharp per-sub-pixel luma step, then clamp. */
                c = pack565(clamp_step(textured_pixel(blk->word0, blk->word4, ph)));
            } else if (edge_row) {
                /* Top / bottom scanline: no neighbours, blend the colour with itself. */
                c = blend565(colour, colour, colour, ph);
            } else {
                /* Interior flat block: blend toward the phase's H and V neighbours. */
                const Block *hn = &BLOCK(px ? bx + 1 : bx - 1, by);
                const Block *vn = &BLOCK(bx, py ? by + 1 : by - 1);
                c = blend565(colour, neighbour_colour(hn, colour),
                             neighbour_colour(vn, colour), ph);
            }
            out565[oy * W + ox] = c;
        }
    }
    #undef BLOCK
}

/* Per-lane saturating clamp for a flat block's expanded word at build time
 * (DEC130.DLL 0x1000cde8): underflow clears a lane to 0, overflow saturates it. */
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

/* Expanded colour word ("word4") for block bi, from the DLL colour tables
 * (spec § 9.1). Textured blocks store the raw sum; flat blocks are clamped. */
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

/* The RGB888 a flat block with the given (yavg, cb, cr) renders to as a uniform
 * interior pixel -- the forward colour map an encoder inverts to choose a block's
 * luma/chroma from a target colour. */
void replay_esc130_flat_rgb(unsigned yavg, unsigned cb, unsigned cr, uint8_t out[3])
{
    uint32_t sum = R_CR[cr & 63u] + R_CB[cb & 0xFFu]
                 + (R_LUM[yavg & 0x3Fu] | 0x80000000u);
    uint32_t d = clamp_build(sum) & COLOUR_MASK;
    uint16_t c = blend565(d, d, d, 0);
    unsigned r5 = (c >> 11) & 0x1Fu, g6 = (c >> 5) & 0x3Fu, b5 = c & 0x1Fu;
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* ---- public API ----------------------------------------------------------- */
ReplayEsc130 *replay_esc130_open(unsigned width, unsigned height)
{
    ReplayEsc130 *s;
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
    s->rbuf = calloc(s->total + 1, sizeof(Block));
    s->out565 = calloc((size_t)s->w * (size_t)s->h, sizeof(uint16_t));
    if (s->blk == NULL || s->tex == NULL || s->rbuf == NULL || s->out565 == NULL) {
        replay_esc130_close(s);
        return NULL;
    }
    esc130_build_sign_tbl(s->sign_tbl);
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
    render_blocks(s->rbuf, s->w, s->h, s->out565);

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

/* Block-state accessors: the persistent per-block word (word0) and the "textured"
 * flag, plus the block count. The encoder re-codes these to reproduce a frame. */
const uint32_t *replay_esc130_blocks(const ReplayEsc130 *s) { return s->blk; }
const uint8_t *replay_esc130_textured(const ReplayEsc130 *s) { return s->tex; }
size_t replay_esc130_block_count(const ReplayEsc130 *s) { return s->total; }
