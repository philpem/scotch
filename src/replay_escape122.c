/*
 * replay_escape122.c -- Eidos/Acorn "Escape 122" (Replay video format 122).
 * See include/replay/replay_escape122.h.
 *
 * Escape 122 is a palettised (PAL8) delta codec, unrelated to escape124/130 -- the
 * proprietary Windows Streamer DLLs only decode 124 and 130; only the original
 * Eidos DOS player decoded 122. This decoder implements the format documented in
 * docs/spec/eidos-escape.md: 8x8 superblocks of 4x4 2x2 macroblocks, an LSB-first
 * bitstream, a per-chunk VGA palette, with the frame and palette persisting across
 * frames. (NihAV's Escape122 decoder was a reference for the format.)
 */
#include "replay/replay_escape122.h"

#include <stdlib.h>
#include <string.h>

/* ---- LE bit reader (LSB-first within each byte) --------------------------- */
typedef struct {
    const uint8_t *buf;
    size_t len;        /* bytes */
    size_t bitpos;     /* next bit to read */
    int overrun;
} BitR;

static void br_init(BitR *b, const uint8_t *buf, size_t len)
{
    b->buf = buf; b->len = len; b->bitpos = 0; b->overrun = 0;
}
static uint32_t br_read(BitR *b, unsigned n)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        size_t p = b->bitpos + i;
        unsigned bit;
        if ((p >> 3) < b->len) bit = (b->buf[p >> 3] >> (p & 7)) & 1;
        else { bit = 0; b->overrun = 1; }
        v |= (uint32_t)bit << i;
    }
    b->bitpos += n;
    return v;
}
static unsigned br_bool(BitR *b) { return br_read(b, 1); }

/* RICE-style skip-run VLC: a returned N means skip N superblocks. */
static size_t read_ecode(BitR *b)
{
    unsigned v3, v7, v12;
    if (!br_bool(b)) return 0;
    v3 = br_read(b, 3);
    if (v3 != 7) return (size_t)v3 + 1;
    v7 = br_read(b, 7);
    if (v7 != 127) return (size_t)v7 + 1 + 7;
    v12 = br_read(b, 12);
    return (size_t)v12 + 1 + 7 + 127;
}

/* One 2x2 macroblock -> px[0..3] = TL,TR,BL,BR palette indices. */
static void read_blk2x2(BitR *b, uint8_t px[4])
{
    unsigned mask = br_read(b, 4);
    if (mask == 0x0) {
        uint8_t idx = (uint8_t)(br_read(b, 7) * 2);          /* uniform, even */
        px[0] = px[1] = px[2] = px[3] = idx;
    } else if (mask == 0xF) {
        uint8_t idx = (uint8_t)(br_read(b, 7) * 2 + 1);      /* uniform, odd  */
        px[0] = px[1] = px[2] = px[3] = idx;
    } else {
        uint8_t clr[2];
        clr[0] = (uint8_t)br_read(b, 8);
        clr[1] = (uint8_t)br_read(b, 8);
        px[0] = clr[mask & 1];
        px[1] = clr[(mask >> 1) & 1];
        px[2] = clr[(mask >> 2) & 1];
        px[3] = clr[(mask >> 3) & 1];
    }
}

struct ReplayEsc122 {
    int W, H;
    uint8_t *frame;     /* W*H palette indices, persistent */
    uint8_t pal[768];   /* 256 * RGB, 8-bit, persistent    */
    int offsets[16];
};

ReplayEsc122 *replay_esc122_open(unsigned width, unsigned height)
{
    ReplayEsc122 *s;
    int i;
    if (width == 0 || height == 0)
        return NULL;
    s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->W = (int)width; s->H = (int)height;
    s->frame = calloc((size_t)width * height, 1);
    if (s->frame == NULL) { free(s); return NULL; }
    for (i = 0; i < 16; i++)
        s->offsets[i] = (i & 3) * 2 + (i >> 2) * 2 * s->W;
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
    unsigned pal_size, nentries;
    size_t off;
    BitR b;
    int is_intra = 1, W, sy, x;
    size_t skip = 0;
    int new_skip = 0;
    const int *offs;

    if (s == NULL || chunk == NULL || clen < 10)
        return -1;
    cid = (uint32_t)chunk[0] | ((uint32_t)chunk[1] << 8)
        | ((uint32_t)chunk[2] << 16) | ((uint32_t)chunk[3] << 24);
    /* chunk[4..7] = vsize (unused here) */
    pal_size = chunk[8] | ((unsigned)chunk[9] << 8);
    if (cid != 0x116)
        return -1;

    off = 10;
    nentries = pal_size / 3;
    if (nentries > 256) nentries = 256;
    if (nentries > 0 && off + (size_t)nentries * 3 <= clen) {
        unsigned i;
        for (i = 0; i < nentries * 3; i++) {
            uint8_t v = chunk[off + i];
            s->pal[i] = (uint8_t)((v << 2) | (v >> 4));   /* 6 -> 8 bits */
        }
    }
    off += pal_size;                 /* skip the whole palette block */
    if (off > clen)
        return -1;

    br_init(&b, chunk + off, clen - off);
    W = s->W;
    offs = s->offsets;

    for (sy = 0; sy < s->H; sy += 8) {
        uint8_t *strip = s->frame + (size_t)sy * (size_t)W;
        for (x = 0; x < W; x += 8) {
            if (!new_skip) { skip = read_ecode(&b); new_skip = 1; }
            if (skip > 0) { skip--; is_intra = 0; continue; }

            /* pass A: broadcast one block to all macroblocks set in the mask */
            while (!br_bool(&b)) {
                uint8_t blk[4];
                unsigned mask, i;
                read_blk2x2(&b, blk);
                mask = br_read(&b, 16);
                for (i = 0; i < 16; i++) {
                    if (mask & 1) {
                        uint8_t *p = strip + offs[i] + x;
                        p[0] = blk[0]; p[1] = blk[1];
                        p[W] = blk[2]; p[W + 1] = blk[3];
                    }
                    mask >>= 1;
                }
                if (b.overrun) goto done;
            }
            /* pass B: a fresh block per masked macroblock */
            if (!br_bool(&b)) {
                unsigned mask = br_read(&b, 16), i;
                for (i = 0; i < 16; i++) {
                    if (mask & 1) {
                        uint8_t blk[4];
                        uint8_t *p = strip + offs[i] + x;
                        read_blk2x2(&b, blk);
                        p[0] = blk[0]; p[1] = blk[1];
                        p[W] = blk[2]; p[W + 1] = blk[3];
                    }
                    mask >>= 1;
                }
            }
            new_skip = 0;
            if (b.overrun) goto done;
        }
    }
done:
    return is_intra;
}

const uint8_t *replay_esc122_frame(const ReplayEsc122 *s) { return s->frame; }
const uint8_t *replay_esc122_palette(const ReplayEsc122 *s) { return s->pal; }
