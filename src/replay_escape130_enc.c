/*
 * replay_escape130_enc.c -- state-level encoder for Eidos/Acorn "Escape 2.0" (130).
 * See include/replay/replay_escape130_enc.h and the decoder replay_escape130.c,
 * whose per-block luma/chroma modes this inverts (docs/spec/eidos-escape.md
 * § Type 130).
 *
 * Each block is a packed 32-bit state: yavg (bits 0-5), a texture step (6-7), four
 * 2-bit texture signs (8-15), and a chroma pair cb/cr (16-20 / 24-28). A frame
 * codes the blocks in raster order, each predicted from its left neighbour's
 * current state; unchanged blocks (vs the previous frame) are skipped. For each
 * coded block we pick the luma and chroma prefix modes that reproduce its target
 * state exactly from the predictor:
 *   luma   : SIGNS (a fresh texture), COPY, DELTA (+/- a small step) or ABS;
 *   chroma : COPY, DELTA (a packed +/- step) or ABS.
 * A block equal to its predictor is a "full copy" (COPY luma + COPY chroma, which
 * the decoder special-cases to return the predictor verbatim, keeping its texture
 * bits). Because that special case also clears the "textured" flag, a textured
 * block must use SIGNS, and a non-textured block equal to its predictor uses the
 * full copy; everything else avoids the accidental full-copy combination.
 */
#include "replay/replay_escape130_enc.h"
#include "replay/replay_escape130.h"    /* replay_esc130_flat_rgb (colour map) */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "escape130_tables.h"

/* ---- LSB-first bit writer (mirrors the decoder's reader) -------------------*/
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bitpos;
    int overflow;
} BitW;

static void wput(BitW *w, unsigned v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        size_t byte = (w->bitpos >> 3);
        if (byte >= w->cap) { w->overflow = 1; w->bitpos++; continue; }
        if ((v >> i) & 1u) w->buf[byte] |= (uint8_t)(1u << (w->bitpos & 7));
        w->bitpos++;
    }
}

/* Skip-run VLC: 1 bit for 0, else an escalating 3/8/15-bit tail -- the inverse of
 * the decoder's read_skip(). */
static void emit_skip(BitW *w, unsigned run)
{
    if (run == 0) { wput(w, 1, 1); return; }
    wput(w, 0, 1);
    if (run <= 7)   { wput(w, run, 3); return; }           /* 1..7    */
    wput(w, 0, 3);
    if (run <= 262) { wput(w, run - 7, 8); return; }        /* 8..262  */
    wput(w, 0, 8);
    wput(w, run - 262, 15);                                 /* 263..   */
}

/* ---- per-block mode selection ---------------------------------------------*/
enum { LUMA_SIGNS, LUMA_COPY, LUMA_DELTA, LUMA_ABS };
enum { CHROMA_COPY, CHROMA_DELTA, CHROMA_ABS };

/* A flat block state is (yavg 0..63, cb 0..31, cr 0..31), packed as an index
 * yavg<<10 | cb<<5 | cr (0..65535) for the reverse colour map below. */
#define FLAT_IDX(yavg, cb, cr) (((yavg) << 10) | ((cb) << 5) | (cr))
#define FLAT_NCAND 65536

struct ReplayEsc130Enc {
    int w, h, bw, bh;
    size_t total;
    uint32_t *recon;      /* per-block reconstructed word0 (persistent) */
    uint8_t *recon_tex;   /* per-block textured flag (persistent) */
    uint32_t sign_tbl[64];
    int rev_sign[256];    /* texture byte (word bits 8-15) -> a SIGNS sidx, or -1 */
    /* RGB -> flat-state inversion (built at open): the RGB each of the 65536 flat
     * states renders to, and a coarse RGB(>>3 per channel)->state cell lookup. */
    uint8_t *cand_rgb;    /* FLAT_NCAND * 3 */
    uint16_t *lut;        /* 32*32*32 -> a flat index */
    uint32_t *sw0;        /* scratch: per-block word0 */
    uint8_t *stex;        /* scratch: per-block textured (all 0 for flat) */
};

/* Emit one coded block, reproducing target state `T` (with textured flag `tt`)
 * from predictor `P`. */
static void emit_block(ReplayEsc130Enc *e, BitW *w, uint32_t T, uint32_t P, int tt)
{
    unsigned LT = T & 0xFFFFu;             /* luma half (bits 0-15)   */
    uint32_t CT = T & 0xFFFF0000u;         /* chroma half (bits 16-31) */
    int luma, chroma;
    unsigned sidx = 0, step = 0, ya = 0, dl = 0, dc = 0;
    int d;

    /* ---- choose the luma mode ---- */
    if (tt) {
        /* Textured blocks can only come from SIGNS. Recover the texture pattern
         * index from the packed signs, the step and the (even) base luma. */
        luma = LUMA_SIGNS;
        sidx = (unsigned)e->rev_sign[(LT >> 8) & 0xFFu];
        step = (LT >> 6) & 3u;
        ya   = (LT >> 1) & 0x1Fu;
    } else if (T == P) {
        /* Unchanged from the predictor: a full copy (see chroma below). */
        luma = LUMA_COPY;
    } else {
        /* A flat block that differs from the predictor: reproduce its luma half
         * (bits 0-15, with the texture bits already zero) exactly. */
        luma = LUMA_ABS;                                   /* always valid */
        if ((P & 0x3Fu) == LT) {
            luma = LUMA_COPY;                              /* left luma matches */
        } else {
            for (d = 0; d < 8; d++) {
                int yv = (int)(P & 0x3Fu) + ESC130_Y_DIFF[d];
                if (yv >= 0 && (unsigned)yv == LT) { luma = LUMA_DELTA; dl = (unsigned)d; break; }
            }
        }
    }

    /* ---- choose the chroma mode ---- */
    if ((P & 0xFFFF0000u) == CT) {
        chroma = CHROMA_COPY;
    } else {
        chroma = CHROMA_ABS;                               /* clean cb/cr, no spill */
        for (d = 0; d < 8; d++)
            if (((P & 0xFFFF0000u) + ESC130_C_DIFF[d]) == CT) {
                chroma = CHROMA_DELTA; dc = (unsigned)d; break;
            }
    }

    /* COPY luma + COPY chroma is the decoder's "full copy" special case, which
     * returns the predictor. We only want that when the block truly equals the
     * predictor; otherwise force the luma to ABS (which reproduces the same flat
     * luma without triggering the special case). */
    if (luma == LUMA_COPY && chroma == CHROMA_COPY && T != P)
        luma = LUMA_ABS;

    /* ---- emit the luma field ---- */
    switch (luma) {
    case LUMA_SIGNS: wput(w, 1, 1); wput(w, sidx, 6); wput(w, step, 2); wput(w, ya, 5); break;
    case LUMA_COPY:  wput(w, 0, 1); wput(w, 0, 1); break;                    /* "00" */
    case LUMA_DELTA: wput(w, 0, 1); wput(w, 1, 1); wput(w, 0, 1); wput(w, dl, 3); break; /* "010" */
    default:         wput(w, 0, 1); wput(w, 1, 1); wput(w, 1, 1);            /* "011" ABS */
                     wput(w, LT & 0x3Fu, 6); break;
    }
    /* ---- emit the chroma field ---- */
    switch (chroma) {
    case CHROMA_COPY:  wput(w, 0, 1); break;                                 /* "0"  */
    case CHROMA_DELTA: wput(w, 1, 1); wput(w, 0, 1); wput(w, dc, 3); break;  /* "10" */
    default:           wput(w, 1, 1); wput(w, 1, 1);                         /* "11" ABS */
                       wput(w, (CT >> 16) & 0x1Fu, 5); wput(w, (CT >> 24) & 0x1Fu, 5); break;
    }
}

/* Build the RGB -> flat-state inversion: the RGB each of the 65536 flat states
 * renders to (via the decoder's colour path), and a coarse 32x32x32 RGB-cell LUT
 * giving, per cell, the state whose RGB is nearest the cell centre. Cells no state
 * lands in (colours outside the codec's gamut) are flood-filled from neighbours.
 * Built once at open. */
static void build_flat_lut(ReplayEsc130Enc *e)
{
    int *celld = malloc((size_t)32 * 32 * 32 * sizeof(int));
    unsigned yavg, cb, cr;
    int cell, pass;
    if (celld == NULL) return;
    for (cell = 0; cell < 32 * 32 * 32; cell++) { celld[cell] = INT_MAX; e->lut[cell] = 0; }

    for (yavg = 0; yavg < 64; yavg++)
        for (cb = 0; cb < 32; cb++)
            for (cr = 0; cr < 32; cr++) {
                unsigned idx = FLAT_IDX(yavg, cb, cr);
                uint8_t *c = &e->cand_rgb[(size_t)idx * 3];
                int R, G, B, cc, dc;
                replay_esc130_flat_rgb(yavg, cb, cr, c);
                R = c[0] >> 3; G = c[1] >> 3; B = c[2] >> 3;
                cc = (R << 10) | (G << 5) | B;
                dc = (c[0]-(R*8+4))*(c[0]-(R*8+4)) + (c[1]-(G*8+4))*(c[1]-(G*8+4))
                   + (c[2]-(B*8+4))*(c[2]-(B*8+4));
                if (dc < celld[cc]) { celld[cc] = dc; e->lut[cc] = (uint16_t)idx; }
            }

    for (pass = 0; pass < 96; pass++) {
        int changed = 0, R, G, B;
        for (R = 0; R < 32; R++) for (G = 0; G < 32; G++) for (B = 0; B < 32; B++) {
            int nb[6], nn = 0, k;
            cell = (R << 10) | (G << 5) | B;
            if (celld[cell] != INT_MAX) continue;
            if (R > 0)  nb[nn++] = ((R-1) << 10) | (G << 5) | B;
            if (R < 31) nb[nn++] = ((R+1) << 10) | (G << 5) | B;
            if (G > 0)  nb[nn++] = (R << 10) | ((G-1) << 5) | B;
            if (G < 31) nb[nn++] = (R << 10) | ((G+1) << 5) | B;
            if (B > 0)  nb[nn++] = (R << 10) | (G << 5) | (B-1);
            if (B < 31) nb[nn++] = (R << 10) | (G << 5) | (B+1);
            for (k = 0; k < nn; k++)
                if (celld[nb[k]] < INT_MAX) {
                    e->lut[cell] = e->lut[nb[k]];
                    celld[cell] = INT_MAX - 1;      /* filled this pass */
                    changed = 1; break;
                }
        }
        if (!changed) break;
    }
    free(celld);
}

/* ---- public API -----------------------------------------------------------*/
ReplayEsc130Enc *replay_esc130enc_open(unsigned width, unsigned height)
{
    ReplayEsc130Enc *e;
    int i;
    if (width == 0 || height == 0 || (width % 2) || (height % 2))
        return NULL;
    e = calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    e->w = (int)width; e->h = (int)height;
    e->bw = (int)(width / 2); e->bh = (int)(height / 2);
    e->total = (size_t)e->bw * (size_t)e->bh;
    e->recon = calloc(e->total ? e->total : 1, sizeof(uint32_t));
    e->recon_tex = calloc(e->total ? e->total : 1, 1);
    e->cand_rgb = malloc((size_t)FLAT_NCAND * 3);
    e->lut = malloc((size_t)32 * 32 * 32 * sizeof(uint16_t));
    e->sw0 = calloc(e->total ? e->total : 1, sizeof(uint32_t));
    e->stex = calloc(e->total ? e->total : 1, 1);
    if (e->recon == NULL || e->recon_tex == NULL || e->cand_rgb == NULL ||
        e->lut == NULL || e->sw0 == NULL || e->stex == NULL) {
        replay_esc130enc_close(e); return NULL;
    }
    /* Build the sign table and the reverse map (texture byte -> a sidx). */
    esc130_build_sign_tbl(e->sign_tbl);
    for (i = 0; i < 256; i++) e->rev_sign[i] = -1;
    for (i = 0; i < 64; i++) {
        unsigned tb = (e->sign_tbl[i] >> 8) & 0xFFu;
        if (e->rev_sign[tb] < 0) e->rev_sign[tb] = i;      /* first sidx wins */
    }
    build_flat_lut(e);
    return e;
}

void replay_esc130enc_close(ReplayEsc130Enc *e)
{
    if (e == NULL) return;
    free(e->recon);
    free(e->recon_tex);
    free(e->cand_rgb);
    free(e->lut);
    free(e->sw0);
    free(e->stex);
    free(e);
}

size_t replay_esc130enc_frame(ReplayEsc130Enc *e, const uint32_t *word0,
                              const uint8_t *textured, uint8_t *out, size_t cap)
{
    BitW w;
    size_t i, nbytes, total;
    long last = -1;

    if (e == NULL || word0 == NULL || textured == NULL || out == NULL || cap < 20)
        return 0;

    /* 16-byte chunk header: [u16 magic 0x130][u16 flags][u32 vsize][8 reserved]. */
    memset(out, 0, cap);
    out[0] = 0x30; out[1] = 0x01;

    /* Bitstream at chunk+16. Walk blocks in raster order: skip a block unchanged
     * from the previous frame, else flush the run of skips before it and code it,
     * predicting from the left neighbour's current reconstruction. */
    w.buf = out + 16; w.cap = cap - 16; w.bitpos = 0; w.overflow = 0;
    for (i = 0; i < e->total; i++) {
        uint32_t T = word0[i], P;
        int tt = textured[i] ? 1 : 0;
        if (T == e->recon[i] && tt == (e->recon_tex[i] ? 1 : 0))
            continue;                                       /* unchanged -> skip */
        emit_skip(&w, (unsigned)((long)i - (last + 1)));    /* run of skips before it */
        P = (i > 0) ? e->recon[i - 1] : 0u;                 /* left neighbour / seed */
        emit_block(e, &w, T, P, tt);
        e->recon[i] = T;
        e->recon_tex[i] = (uint8_t)tt;
        last = (long)i;
    }
    emit_skip(&w, (unsigned)((long)e->total - (last + 1))); /* trailing run to the end */

    if (w.overflow)
        return 0;
    nbytes = (w.bitpos + 7) / 8;
    total = 16 + nbytes;
    out[4] = (uint8_t)total; out[5] = (uint8_t)(total >> 8);
    out[6] = (uint8_t)(total >> 16); out[7] = (uint8_t)(total >> 24);
    return total;
}

size_t replay_esc130enc_frame_rgb(ReplayEsc130Enc *e, const uint8_t *rgb,
                                  uint8_t *out, size_t cap)
{
    int bx, by;
    if (e == NULL || rgb == NULL || out == NULL)
        return 0;

    /* Turn each 2x2 source region into one flat block state: average its colour,
     * look it up in the coarse LUT, then refine against nearby candidates for the
     * exact nearest representable colour. */
    for (by = 0; by < e->bh; by++)
        for (bx = 0; bx < e->bw; bx++) {
            int r = 0, g = 0, b = 0, dy, dx;
            unsigned base;
            int by0, cb0, cr0, bestd, yy, cc, rr;
            unsigned best;
            for (dy = 0; dy < 2; dy++)
                for (dx = 0; dx < 2; dx++) {
                    size_t p = ((size_t)(by * 2 + dy) * (size_t)e->w
                              + (size_t)(bx * 2 + dx)) * 3;
                    r += rgb[p]; g += rgb[p + 1]; b += rgb[p + 2];
                }
            r = (r + 2) / 4; g = (g + 2) / 4; b = (b + 2) / 4;

            base = e->lut[((size_t)(r >> 3) << 10) | ((size_t)(g >> 3) << 5) | (size_t)(b >> 3)];
            by0 = (int)(base >> 10); cb0 = (int)((base >> 5) & 31u); cr0 = (int)(base & 31u);
            bestd = INT_MAX; best = base;
            for (yy = by0 - 3; yy <= by0 + 3; yy++) {
                if (yy < 0 || yy > 63) continue;
                for (cc = cb0 - 2; cc <= cb0 + 2; cc++) {
                    if (cc < 0 || cc > 31) continue;
                    for (rr = cr0 - 2; rr <= cr0 + 2; rr++) {
                        const uint8_t *c;
                        int d;
                        if (rr < 0 || rr > 31) continue;
                        c = &e->cand_rgb[(size_t)FLAT_IDX((unsigned)yy,
                                          (unsigned)cc, (unsigned)rr) * 3];
                        d = (r-c[0])*(r-c[0]) + (g-c[1])*(g-c[1]) + (b-c[2])*(b-c[2]);
                        if (d < bestd) { bestd = d; best = FLAT_IDX((unsigned)yy,
                                          (unsigned)cc, (unsigned)rr); }
                    }
                }
            }
            e->sw0[(size_t)by * (size_t)e->bw + (size_t)bx] =
                (best >> 10) | (((best >> 5) & 31u) << 16) | ((best & 31u) << 24);
            e->stex[(size_t)by * (size_t)e->bw + (size_t)bx] = 0;
        }

    return replay_esc130enc_frame(e, e->sw0, e->stex, out, cap);
}

const uint32_t *replay_esc130enc_recon(const ReplayEsc130Enc *e) { return e->recon; }
