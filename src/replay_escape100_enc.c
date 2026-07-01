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

/* ---- LSB-first bit writer --------------------------------------------------*/
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bitpos;
    int overflow;
} BitW;

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

/* Inverse of the decoder's variable-length fields. */
static void emit_skip(BitW *w, unsigned run)
{
    if (run == 0) { wput(w, 0, 1); return; }
    wput(w, 1, 1);
    if (run <= 7)   { wput(w, run - 1, 3); return; }
    wput(w, 7, 3);
    if (run <= 134) { wput(w, run - 8, 7); return; }
    wput(w, 127, 7);
    wput(w, run - 135, 15);
}
static void emit_chroma_index(BitW *w, unsigned idx)
{
    if (idx <= 48) { wput(w, idx, 6); return; }
    wput(w, idx & 63u, 6);
    wput(w, idx >> 6, 2);
}
static void emit_luma(BitW *w, unsigned sel, unsigned lumaA, unsigned lumaB,
                      int newchroma, unsigned chroma_idx)
{
    wput(w, 1, 1);              /* mode = luma */
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

/* Nearest reachable codebook entry to a target (u,v) chroma. */
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

/* Pick luma A (= top-left), luma B, and the 3-bit selector minimising the L1
 * luma error over the three non-top-left sub-pixels. */
static void pick_luma(unsigned y0, unsigned y1, unsigned y2, unsigned y3,
                      unsigned *A, unsigned *B, unsigned *sel)
{
    unsigned yk[3] = { y1, y2, y3 };
    int a = (int)y0, best_err, best_B = (int)y0, best_sel = 0, c;
    best_err = iabs((int)y1 - a) + iabs((int)y2 - a) + iabs((int)y3 - a); /* sel 0 */
    for (c = 0; c < 3; c++) {
        int b = (int)yk[c], err = 0, s = 0, k;
        for (k = 0; k < 3; k++) {
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
    /* reachable chroma indices: 0..48 (6-bit), then 49..63 + hi*64 (8-bit). */
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

    for (cursor = 0; cursor < total; cursor++) {
        int bx = cursor % e->bw, by = cursor / e->bw;
        size_t tl = (size_t)(by * 2) * (size_t)e->w + (size_t)(bx * 2);
        size_t bl = tl + (size_t)e->w;
        uint16_t s0 = src[tl], s1 = src[tl + 1], s2 = src[bl], s3 = src[bl + 1];
        unsigned A, B, sel, idx;
        uint16_t tchroma, rl[4], np[4], old_chroma, chroma_used;
        int newchroma, uu, vv;

        pick_luma(s0 & YMASK, s1 & YMASK, s2 & YMASK, s3 & YMASK, &A, &B, &sel);
        uu = (int)((((s0 >> 5) & YMASK) + ((s1 >> 5) & YMASK)
            + ((s2 >> 5) & YMASK) + ((s3 >> 5) & YMASK) + 2) / 4);
        vv = (int)((((s0 >> 10) & YMASK) + ((s1 >> 10) & YMASK)
            + ((s2 >> 10) & YMASK) + ((s3 >> 10) & YMASK) + 2) / 4);
        idx = nearest_chroma(e, uu, vv);
        tchroma = ESC100_CHROMA[idx];

        rl[0] = (uint16_t)A;
        rl[1] = (uint16_t)((sel & 1) ? B : A);
        rl[2] = (uint16_t)((sel & 2) ? B : A);
        rl[3] = (uint16_t)((sel & 4) ? B : A);
        np[0] = (uint16_t)(tchroma | rl[0]);
        np[1] = (uint16_t)(tchroma | rl[1]);
        np[2] = (uint16_t)(tchroma | rl[2]);
        np[3] = (uint16_t)(tchroma | rl[3]);

        if (np[0] == e->recon[tl] && np[1] == e->recon[tl + 1] &&
            np[2] == e->recon[bl] && np[3] == e->recon[bl + 1])
            continue;                                   /* skip: no change */

        old_chroma = (uint16_t)(e->recon[tl] & ~YMASK);
        newchroma = (tchroma == old_chroma) ? 0 : 1;
        chroma_used = newchroma ? tchroma : old_chroma;

        emit_skip(&w, (unsigned)(cursor - (last + 1)));
        emit_luma(&w, sel, A, B, newchroma, idx);

        e->recon[tl]      = (uint16_t)(chroma_used | rl[0]);
        e->recon[tl + 1]  = (uint16_t)(chroma_used | rl[1]);
        e->recon[bl]      = (uint16_t)(chroma_used | rl[2]);
        e->recon[bl + 1]  = (uint16_t)(chroma_used | rl[3]);
        last = cursor;
    }
    emit_skip(&w, (unsigned)(total - (last + 1)));      /* run to the end */

    if (w.overflow)
        return 0;
    nbytes = (w.bitpos + 7) / 8;
    while (nbytes % 4) nbytes++;                        /* word-align the frame */
    return hdr + nbytes;
}

const uint16_t *replay_esc100enc_recon(const ReplayEsc100Enc *e) { return e->recon; }
