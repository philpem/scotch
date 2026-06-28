/*
 * replay_escape122.c -- Eidos/Acorn "Escape 122" (Replay video format 122).
 * See include/replay/replay_escape122.h.
 *
 * Implemented from scratch, strictly from the behavioural specification in
 * docs/spec/eidos-escape.md (§ Type 122). No existing decoder implementation was
 * consulted. Escape 122 is a palettised (PAL8) delta codec, unrelated to the
 * RGB555 codec 124 or the YUV codec 130: the proprietary Eidos Streamer DLLs
 * cannot decode it (only the original DOS player could).
 */
#include "replay/replay_escape122.h"

#include <stdlib.h>
#include <string.h>

/* ---- bit reader: LSB-first within each byte (spec §3) --------------------- */
typedef struct {
    const uint8_t *data;
    size_t len;        /* bytes available */
    size_t bytepos;
    int bitpos;        /* 0..7 */
    int eof;           /* set once a read goes past the end */
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

/* ---- skip-run VLC (spec §7) ----------------------------------------------- */
static int read_skip_run(BitReader *b)
{
    unsigned v3, v7, v12;
    if (rd_bit(b) == 0) return 0;
    if (b->eof) return 0;
    v3 = rd_bits(b, 3);
    if (b->eof) return 0;
    if (v3 != 7) return (int)v3 + 1;
    v7 = rd_bits(b, 7);
    if (b->eof) return 0;
    if (v7 != 127) return (int)v7 + 1 + 7;
    v12 = rd_bits(b, 12);
    if (b->eof) return 0;
    return (int)v12 + 1 + 7 + 127;
}

/* ---- 2x2 colour block: TL,TR,BL,BR palette indices (spec §8) --------------- */
typedef struct { uint8_t c[4]; } Block;

static Block read_block(BitReader *b)
{
    Block blk;
    unsigned mk = rd_bits(b, 4);
    if (b->eof) { blk.c[0] = blk.c[1] = blk.c[2] = blk.c[3] = 0; return blk; }
    if (mk == 0x0) {                       /* uniform, even index */
        uint8_t v = (uint8_t)(2 * rd_bits(b, 7));
        blk.c[0] = blk.c[1] = blk.c[2] = blk.c[3] = v;
    } else if (mk == 0xF) {                /* uniform, odd index */
        uint8_t v = (uint8_t)(2 * rd_bits(b, 7) + 1);
        blk.c[0] = blk.c[1] = blk.c[2] = blk.c[3] = v;
    } else {                               /* two indices, per-pixel mask */
        uint8_t c0 = (uint8_t)rd_bits(b, 8);
        uint8_t c1 = (uint8_t)rd_bits(b, 8);
        int k;
        for (k = 0; k < 4; k++)
            blk.c[k] = ((mk >> k) & 1) ? c1 : c0;
    }
    return blk;
}

struct ReplayEsc122 {
    int W, H;
    uint8_t *frame;     /* W*H palette indices, persistent */
    uint8_t pal[768];   /* 256 * RGB, 8-bit, persistent    */
};

/* Write a 2x2 block to macroblock m (4x4 grid) of the superblock at (sbx,sby). */
static void write_block(ReplayEsc122 *s, const Block *b, int sbx, int sby, int m)
{
    size_t W = (size_t)s->W;
    size_t x = (size_t)(sbx + 2 * (m & 3));
    size_t y = (size_t)(sby + 2 * (m >> 2));
    uint8_t *f = s->frame;
    size_t r0 = y * W + x;        /* top row    */
    size_t r1 = (y + 1) * W + x;  /* bottom row */
    f[r0]     = b->c[0]; /* TL */
    f[r0 + 1] = b->c[1]; /* TR */
    f[r1]     = b->c[2]; /* BL */
    f[r1 + 1] = b->c[3]; /* BR */
}

/* Decode one coded superblock at pixel origin (sbx,sby) (spec §9). */
static void decode_superblock(ReplayEsc122 *s, BitReader *b, int sbx, int sby)
{
    int m;
    /* Pass 1: broadcast identical blocks until a stop bit (==1). */
    for (;;) {
        int stop = rd_bit(b);
        if (b->eof) return;
        if (stop == 1) break;
        {
            Block blk = read_block(b);
            unsigned mask;
            if (b->eof) return;
            mask = rd_bits(b, 16);
            if (b->eof) return;
            for (m = 0; m < 16; m++)
                if ((mask >> m) & 1)
                    write_block(s, &blk, sbx, sby, m);
        }
    }
    /* Pass 2: one fresh block per selected macroblock (present bit == 0). */
    if (rd_bit(b) == 0 && !b->eof) {
        unsigned mask = rd_bits(b, 16);
        if (b->eof) return;
        for (m = 0; m < 16; m++) {
            if ((mask >> m) & 1) {
                Block blk = read_block(b);
                if (b->eof) return;
                write_block(s, &blk, sbx, sby, m);
            }
        }
    }
}

/* ---- public API ----------------------------------------------------------- */
ReplayEsc122 *replay_esc122_open(unsigned width, unsigned height)
{
    ReplayEsc122 *s;
    if (width == 0 || height == 0)
        return NULL;
    s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->W = (int)width; s->H = (int)height;
    s->frame = calloc((size_t)width * height, 1);
    if (s->frame == NULL) { free(s); return NULL; }
    return s;
}

void replay_esc122_close(ReplayEsc122 *s)
{
    if (s == NULL)
        return;
    free(s->frame);
    free(s);
}

int replay_esc122_decode(ReplayEsc122 *s, const uint8_t *chunk, size_t clen)
{
    uint32_t cid;
    unsigned pal_size, entries;
    size_t bs_start;
    BitReader b;
    int sbcols, sbrows, sbx, sby, is_intra = 1;
    int pending = 0, have_pending = 0;

    if (s == NULL || chunk == NULL)
        return -1;
    if (clen < 10)                         /* placeholder chunk: no change */
        return 0;
    cid = (uint32_t)chunk[0] | ((uint32_t)chunk[1] << 8)
        | ((uint32_t)chunk[2] << 16) | ((uint32_t)chunk[3] << 24);
    if (cid != 0x116)
        return -1;
    /* chunk[4..7] = vsize (unused) */
    pal_size = chunk[8] | ((unsigned)chunk[9] << 8);

    /* Palette block (spec §2): 3 bytes/entry, 6-bit components -> 8 bits. A
     * pal_size of 0 keeps the previous palette. */
    if (pal_size > 0) {
        entries = pal_size / 3;
        if (entries > 256) entries = 256;
        if ((size_t)10 + (size_t)entries * 3 <= clen) {
            unsigned i;
            for (i = 0; i < entries * 3; i++) {
                uint8_t v = chunk[10 + i];
                s->pal[i] = (uint8_t)((v << 2) | (v >> 4));
            }
        }
    }

    bs_start = (size_t)10 + pal_size;
    if (bs_start > clen)
        bs_start = clen;
    b.data = chunk + bs_start;
    b.len = clen - bs_start;
    b.bytepos = 0; b.bitpos = 0; b.eof = 0;

    sbcols = s->W / 8;
    sbrows = s->H / 8;
    for (sby = 0; sby < sbrows; sby++) {
        for (sbx = 0; sbx < sbcols; sbx++) {
            if (b.eof) return is_intra;
            if (!have_pending) {
                pending = read_skip_run(&b);
                if (b.eof) return is_intra;
                have_pending = 1;
            }
            if (pending > 0) { pending--; is_intra = 0; continue; }
            decode_superblock(s, &b, sbx * 8, sby * 8);
            if (b.eof) return is_intra;
            have_pending = 0;
        }
    }
    return is_intra;
}

const uint8_t *replay_esc122_frame(const ReplayEsc122 *s) { return s->frame; }
const uint8_t *replay_esc122_palette(const ReplayEsc122 *s) { return s->pal; }
