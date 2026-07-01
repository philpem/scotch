/*
 * replay_escape100_enc.c -- encoder for Eidos/Acorn "Escape" formats 100 and 102.
 * See include/replay/replay_escape100_enc.h.
 *
 * A 2x2-block vector quantiser over the fixed 256-entry chroma codebook (the same
 * table the decoder uses). For each block the encoder picks the nearest codebook
 * chroma and up to two 5-bit luma values (assigned per sub-pixel by a 3-bit
 * selector), keeps the reconstructed picture, and delta-codes: a block whose
 * reconstruction would be unchanged from the previous frame is skipped. The
 * output is format-compatible with the decoder and the Decomp100/102 modules, but
 * the encoder decisions are our own (not bit-compatible with Eidos MovieCompress).
 */
#include "replay/replay_escape100_enc.h"

#include <stdlib.h>
#include <string.h>

#include "escape100_codebook.h"

#define YMASK 0x1Fu

/* ---- LSB-first bit writer (mirrors the decoder's reader) -------------------*/
typedef struct {
    uint8_t *buf;        /* output, assumed pre-zeroed (we OR bits in) */
    size_t cap;          /* capacity in bytes */
    size_t bitpos;       /* next bit position */
    int overflow;        /* set if a write ran past cap */
} BitW;

/* Append the low n bits of v, least-significant bit first. */
static void wput(BitW *w, uint32_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        size_t byte = (w->bitpos >> 3);
        if (byte >= w->cap) { w->overflow = 1; w->bitpos++; continue; }
        if ((v >> i) & 1u) w->buf[byte] |= (uint8_t)(1u << (w->bitpos & 7));
        w->bitpos++;
    }
}

/* The three helpers below write exactly the bits the decoder's read_skip(),
 * read_chroma_index() and luma block reader consume (see replay_escape100.c). */

/* Skip-run VLC: 1 bit for 0, else an escalating 3/7/15-bit tail at each all-ones
 * threshold -- the inverse of read_skip(). */
static void emit_skip(BitW *w, unsigned run)
{
    if (run == 0) { wput(w, 0, 1); return; }        /* one 0 bit = run of 0 */
    wput(w, 1, 1);
    if (run <= 7)   { wput(w, run - 1, 3); return; }        /* 1..7   */
    wput(w, 7, 3);                                          /* escape */
    if (run <= 134) { wput(w, run - 8, 7); return; }        /* 8..134 */
    wput(w, 127, 7);                                        /* escape */
    wput(w, run - 135, 15);                                 /* 135..  */
}
/* Chroma codebook index: 6 bits for 0..48, else 8 bits (a low-6-bit value in
 * 49..63 plus a 2-bit high part). Only indices the VLC can reach are ever passed. */
static void emit_chroma_index(BitW *w, unsigned idx)
{
    if (idx <= 48) { wput(w, idx, 6); return; }
    wput(w, idx & 63u, 6);
    wput(w, idx >> 6, 2);
}
/* A luma block: mode bit 1, the 3-bit selector, luma A, luma B (only when the
 * selector is non-zero, i.e. some sub-pixel actually uses B), then the
 * new-chroma flag and, if set, the chroma index. */
static void emit_luma(BitW *w, unsigned sel, unsigned lumaA, unsigned lumaB,
                      int newchroma, unsigned chroma_idx)
{
    wput(w, 1, 1);              /* mode = luma (0 would be a chroma-only block) */
    wput(w, sel, 3);
    wput(w, lumaA, 5);
    if (sel != 0) wput(w, lumaB, 5);
    wput(w, (unsigned)newchroma, 1);
    if (newchroma) emit_chroma_index(w, chroma_idx);
}

struct ReplayEsc100Enc {
    int w, h, bw, bh;
    unsigned id;
    uint16_t *recon;           /* w*h reconstructed YUV555 */
    /* codebook indices reachable by the chroma-index VLC, with their U/V. */
    unsigned reach[128];
    uint8_t  ru[128], rv[128];
    int nreach;
};

static int iabs(int x) { return x < 0 ? -x : x; }

/* Chroma quantisation: return the codebook index whose U,V is closest (Euclidean)
 * to the target (u,v). A linear scan of the ~109 VLC-reachable entries; because
 * the reachable list is built with the cheaper 6-bit indices (0..48) first, a tie
 * naturally prefers the shorter code. */
static unsigned nearest_chroma(const ReplayEsc100Enc *e, int u, int v)
{
    int best = 0, bestd = 1 << 30, i;
    for (i = 0; i < e->nreach; i++) {
        int du = u - (int)e->ru[i], dv = v - (int)e->rv[i];
        int d = du * du + dv * dv;
        if (d < bestd) { bestd = d; best = i; }
    }
    return e->reach[best];
}

/* Luma quantisation: a block can carry only two luma values, A and B, with the
 * top-left pixel forced to A. Fit the four 5-bit source lumas to that: A is the
 * top-left; B and the selector (which of TR/BL/BR use B) are chosen to minimise
 * the total absolute error over the other three sub-pixels. The best B is one of
 * their actual values, so we just try each and keep the cheapest; a uniform block
 * comes out as selector 0 (all A, no B). */
static void pick_luma(unsigned y0, unsigned y1, unsigned y2, unsigned y3,
                      unsigned *A, unsigned *B, unsigned *sel)
{
    unsigned yk[3] = { y1, y2, y3 };            /* TR, BL, BR */
    int a = (int)y0, best_err, best_B = (int)y0, best_sel = 0, c;
    /* baseline: selector 0 -- every sub-pixel uses A */
    best_err = iabs((int)y1 - a) + iabs((int)y2 - a) + iabs((int)y3 - a);
    for (c = 0; c < 3; c++) {                   /* try each sub-pixel value as B */
        int b = (int)yk[c], err = 0, s = 0, k;
        for (k = 0; k < 3; k++) {               /* assign each sub-pixel to A or B */
            int dA = iabs((int)yk[k] - a), dB = iabs((int)yk[k] - b);
            if (dB < dA) { err += dB; s |= (1 << k); } else { err += dA; }
        }
        if (err < best_err) { best_err = err; best_B = b; best_sel = s; }
    }
    *A = y0; *B = (unsigned)best_B; *sel = (unsigned)best_sel;
}

ReplayEsc100Enc *replay_esc100enc_open(unsigned width, unsigned height,
                                       unsigned codec_id)
{
    ReplayEsc100Enc *e;
    int hi, i6;
    if (width == 0 || height == 0 || (width % 2) || (height % 2))
        return NULL;
    if (codec_id != 0x100 && codec_id != 0x102)
        return NULL;
    e = calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    e->w = (int)width; e->h = (int)height;
    e->bw = (int)(width / 2); e->bh = (int)(height / 2);
    e->id = codec_id;
    e->recon = calloc((size_t)width * height, sizeof(uint16_t));
    if (e->recon == NULL) { free(e); return NULL; }
    /* Build the list of codebook indices the chroma-index VLC can actually
     * encode, so the quantiser only ever chooses a reachable one: 0..48 via the
     * 6-bit form, then a low-6-bit value of 49..63 plus a 2-bit high part (i.e.
     * 49..63, 113..127, 177..191, 241..255) via the 8-bit form. 6-bit ones first
     * so ties pick the cheaper code. */
    e->nreach = 0;
    for (i6 = 0; i6 <= 48; i6++) e->reach[e->nreach++] = (unsigned)i6;
    for (hi = 0; hi < 4; hi++)
        for (i6 = 49; i6 <= 63; i6++)
            e->reach[e->nreach++] = (unsigned)(i6 + hi * 64);
    for (i6 = 0; i6 < e->nreach; i6++) {
        uint16_t c = ESC100_CHROMA[e->reach[i6]];
        e->ru[i6] = (uint8_t)((c >> 5) & YMASK);
        e->rv[i6] = (uint8_t)((c >> 10) & YMASK);
    }
    return e;
}

void replay_esc100enc_close(ReplayEsc100Enc *e)
{
    if (e == NULL) return;
    free(e->recon);
    free(e);
}

size_t replay_esc100enc_frame(ReplayEsc100Enc *e, const uint16_t *src,
                              uint8_t *out, size_t cap)
{
    size_t hdr, nbytes;
    BitW w;
    int cursor, total, last = -1;

    if (e == NULL || src == NULL || out == NULL)
        return 0;
    hdr = (e->id == 0x102) ? 8u : 4u;
    if (cap < hdr + 4)
        return 0;
    memset(out, 0, cap);
    out[0] = (uint8_t)e->id; out[1] = (uint8_t)(e->id >> 8);
    /* out[2..3] and (for 102) out[4..7] are already zero */

    w.buf = out + hdr; w.cap = cap - hdr; w.bitpos = 0; w.overflow = 0;
    total = e->bw * e->bh;

    /* Walk the 2x2 blocks in raster order. For each we work out the best block we
     * can code (its chroma, its 1-2 luma values and selector), and how that would
     * look once decoded ("np", the reconstruction). If that matches what is
     * already on screen ("recon", our copy of the decoder's picture) we leave the
     * block alone; the decoder will do the same via a skip run. Otherwise we emit
     * the block, preceded by a skip run covering the blocks we left alone since
     * the last coded one, and update our reconstruction to match. `last` is the
     * cursor of the previous coded block (-1 before the first). */
    for (cursor = 0; cursor < total; cursor++) {
        int bx = cursor % e->bw, by = cursor / e->bw;
        size_t tl = (size_t)(by * 2) * (size_t)e->w + (size_t)(bx * 2); /* top-left px */
        size_t bl = tl + (size_t)e->w;                                  /* bottom-left */
        uint16_t s0 = src[tl], s1 = src[tl + 1], s2 = src[bl], s3 = src[bl + 1];
        unsigned A, B, sel, idx;
        uint16_t tchroma, rl[4], np[4], old_chroma, chroma_used;
        int newchroma, uu, vv;

        /* Choose the block's luma: A (= top-left) and B, with a 3-bit selector
         * saying which of TR/BL/BR use B. Choose its chroma: the codebook entry
         * nearest the block's average U,V (Y-free, so only U/V matter). */
        pick_luma(s0 & YMASK, s1 & YMASK, s2 & YMASK, s3 & YMASK, &A, &B, &sel);
        uu = (int)((((s0 >> 5) & YMASK) + ((s1 >> 5) & YMASK)
            + ((s2 >> 5) & YMASK) + ((s3 >> 5) & YMASK) + 2) / 4);   /* rounded avg U */
        vv = (int)((((s0 >> 10) & YMASK) + ((s1 >> 10) & YMASK)
            + ((s2 >> 10) & YMASK) + ((s3 >> 10) & YMASK) + 2) / 4);  /* rounded avg V */
        idx = nearest_chroma(e, uu, vv);
        tchroma = ESC100_CHROMA[idx];

        /* Reconstruct the four pixels this block would decode to: each sub-pixel's
         * luma (A or B per the selector) combined with the one block chroma. */
        rl[0] = (uint16_t)A;                        /* TL always uses luma A */
        rl[1] = (uint16_t)((sel & 1) ? B : A);      /* TR */
        rl[2] = (uint16_t)((sel & 2) ? B : A);      /* BL */
        rl[3] = (uint16_t)((sel & 4) ? B : A);      /* BR */
        np[0] = (uint16_t)(tchroma | rl[0]);
        np[1] = (uint16_t)(tchroma | rl[1]);
        np[2] = (uint16_t)(tchroma | rl[2]);
        np[3] = (uint16_t)(tchroma | rl[3]);

        /* Delta coding: if coding the block would not change the picture, skip it
         * (it stays part of the next skip run and keeps its previous-frame value). */
        if (np[0] == e->recon[tl] && np[1] == e->recon[tl + 1] &&
            np[2] == e->recon[bl] && np[3] == e->recon[bl + 1])
            continue;

        /* Coding this block. If its chroma already matches what is on screen, use
         * the luma mode's "keep existing chroma" flag (no chroma index emitted);
         * otherwise send the new chroma index. (Every pixel in a block shares one
         * chroma, so the top-left's chroma represents the whole block.) */
        old_chroma = (uint16_t)(e->recon[tl] & ~YMASK);
        newchroma = (tchroma == old_chroma) ? 0 : 1;
        chroma_used = newchroma ? tchroma : old_chroma;

        /* Emit the run of skipped blocks since the last coded one, then the block. */
        emit_skip(&w, (unsigned)(cursor - (last + 1)));
        emit_luma(&w, sel, A, B, newchroma, idx);

        /* Track the reconstruction so later frames delta against the true picture. */
        e->recon[tl]      = (uint16_t)(chroma_used | rl[0]);
        e->recon[tl + 1]  = (uint16_t)(chroma_used | rl[1]);
        e->recon[bl]      = (uint16_t)(chroma_used | rl[2]);
        e->recon[bl + 1]  = (uint16_t)(chroma_used | rl[3]);
        last = cursor;
    }
    /* A final skip run past the last coded block ends the frame (drives the
     * decoder's cursor past the last block row). */
    emit_skip(&w, (unsigned)(total - (last + 1)));

    if (w.overflow)
        return 0;
    nbytes = (w.bitpos + 7) / 8;
    while (nbytes % 4) nbytes++;                        /* word-align the frame */
    return hdr + nbytes;
}

const uint16_t *replay_esc100enc_recon(const ReplayEsc100Enc *e) { return e->recon; }
