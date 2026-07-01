/*
 * replay_escape122_enc.c -- encoder for Eidos/Acorn "Escape 122" (PAL8).
 * See include/replay/replay_escape122_enc.h and the decoder replay_escape122.c,
 * whose bitstream this inverts (docs/spec/eidos-escape.md § Type 122).
 *
 * The picture is 8-bit palette indices in 8x8 superblocks, each a 4x4 grid of 2x2
 * macroblocks. A macroblock is coded as one index (uniform) or two indices with a
 * per-pixel mask. A superblock lists which of its 16 macroblocks changed and codes
 * those; unchanged superblocks are skipped entirely (a run length). The encoder
 * keeps the reconstructed picture and delta-codes against it, so later frames only
 * spend bits on what actually changed.
 *
 * This first version codes each changed macroblock individually (it does not use
 * the decoder's "broadcast one block to several macroblocks" pass-1 optimisation),
 * which is simplest and still competitive because whole-superblock skipping does
 * most of the temporal work.
 */
#include "replay/replay_escape122_enc.h"

#include <stdlib.h>
#include <string.h>

/* ---- LSB-first bit writer (mirrors the decoder's reader) -------------------*/
typedef struct {
    uint8_t *buf;        /* output, pre-zeroed (bits are OR-ed in) */
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

/* Skip-run VLC: how many whole superblocks to copy from the previous frame. One 0
 * bit for a run of 0, else an escalating 3/7/12-bit tail -- the inverse of the
 * decoder's read_skip_run(). */
static void emit_skip_run(BitW *w, unsigned run)
{
    if (run == 0) { wput(w, 0, 1); return; }
    wput(w, 1, 1);
    if (run <= 7)   { wput(w, run - 1, 3); return; }        /* 1..7    */
    wput(w, 7, 3);
    if (run <= 134) { wput(w, run - 8, 7); return; }        /* 8..134  */
    wput(w, 127, 7);
    wput(w, run - 135, 12);                                 /* 135..4230 */
}

/* ---- 2x2 macroblock coding -------------------------------------------------*/
/* How a 2x2 block will be coded, plus the four indices it decodes back to. */
typedef struct {
    int uniform;         /* 1 = one index (v); 0 = two indices (c0/c1 + mask) */
    uint8_t v, c0, c1;
    unsigned mask;       /* per-pixel: bit k set => pixel k takes c1 (never bit 0) */
    uint8_t recon[4];    /* the reconstructed TL,TR,BL,BR indices */
} MacroCode;

static int pal_dist(const uint8_t *pal, uint8_t a, uint8_t b)
{
    int dr = (int)pal[a * 3]     - (int)pal[b * 3];
    int dg = (int)pal[a * 3 + 1] - (int)pal[b * 3 + 1];
    int db = (int)pal[a * 3 + 2] - (int)pal[b * 3 + 2];
    return dr * dr + dg * dg + db * db;
}
static int idx_dist(const uint8_t *pal, uint8_t a, uint8_t b)
{
    return pal ? pal_dist(pal, a, b) : abs((int)a - (int)b);
}

/* Decide how to code the 2x2 block `src` (TL,TR,BL,BR). One or two distinct
 * indices are represented exactly; three or four are approximated with the two
 * that fit -- c0 is the top-left index (which keeps mask bit 0 clear, so the block
 * never looks "uniform"), c1 is the farthest-away index, and each pixel takes
 * whichever is nearer in palette space. */
static MacroCode code_block(const uint8_t src[4], const uint8_t *pal)
{
    MacroCode mc;
    uint8_t vals[4];
    int nv = 0, k, j;

    for (k = 0; k < 4; k++) {
        int seen = 0;
        for (j = 0; j < nv; j++) if (vals[j] == src[k]) seen = 1;
        if (!seen) vals[nv++] = src[k];
    }

    if (nv == 1) {                                   /* uniform */
        mc.uniform = 1; mc.v = src[0];
        for (k = 0; k < 4; k++) mc.recon[k] = src[0];
        return mc;
    }
    if (nv == 2) {                                   /* exact two-colour */
        mc.uniform = 0;
        mc.c0 = src[0];
        mc.c1 = (vals[0] == src[0]) ? vals[1] : vals[0];
        mc.mask = 0;
        for (k = 0; k < 4; k++) {
            if (src[k] != mc.c0) mc.mask |= (1u << k);
            mc.recon[k] = src[k];
        }
        return mc;
    }

    /* three or four distinct: best two-colour approximation. */
    mc.uniform = 0; mc.c0 = src[0];
    {
        int bestk = 1, bestd = -1;
        for (k = 1; k < 4; k++) {
            int d = idx_dist(pal, src[0], src[k]);
            if (d > bestd) { bestd = d; bestk = k; }
        }
        mc.c1 = src[bestk];
    }
    mc.mask = 0;
    for (k = 0; k < 4; k++) {
        int d0 = idx_dist(pal, src[k], mc.c0), d1 = idx_dist(pal, src[k], mc.c1);
        uint8_t use = (d1 < d0) ? mc.c1 : mc.c0;
        if (use == mc.c1) mc.mask |= (1u << k);
        mc.recon[k] = use;
    }
    if (mc.mask == 0) {                              /* everything chose c0 */
        mc.uniform = 1; mc.v = mc.c0;
        for (k = 0; k < 4; k++) mc.recon[k] = mc.c0;
    }
    return mc;
}

/* Emit one macroblock -- the inverse of the decoder's read_block(). */
static void emit_block(BitW *w, const MacroCode *mc)
{
    if (mc->uniform) {
        /* uniform index v: mk=0 marks an even index, mk=0xF an odd one, and the
         * 7-bit payload is v>>1 either way (2*idx or 2*idx+1 reconstructs v). */
        wput(w, (mc->v & 1u) ? 0xFu : 0x0u, 4);
        wput(w, (unsigned)(mc->v >> 1), 7);
    } else {
        wput(w, mc->mask, 4);            /* 1..14: the per-pixel c0/c1 selector */
        wput(w, mc->c0, 8);
        wput(w, mc->c1, 8);
    }
}

struct ReplayEsc122Enc {
    int W, H, sbcols, sbrows;
    uint8_t *recon;      /* W*H reconstructed indices (decoder's picture) */
    uint8_t pal[768];    /* current 8-bit RGB palette (for approximation) */
    int have_pal;
};

/* Read a superblock's 16 macroblocks from a frame plane. Macroblock m's top-left
 * pixel is at (sbx + 2*(m&3), sby + 2*(m>>2)); its 2x2 goes to out[0..3]. */
static void read_macro(const uint8_t *plane, int W, int sbx, int sby, int m,
                       uint8_t out[4])
{
    int x = sbx + 2 * (m & 3), y = sby + 2 * (m >> 2);
    const uint8_t *r0 = plane + (size_t)y * (size_t)W + (size_t)x;
    const uint8_t *r1 = r0 + W;
    out[0] = r0[0]; out[1] = r0[1]; out[2] = r1[0]; out[3] = r1[1];
}
static void write_macro(uint8_t *plane, int W, int sbx, int sby, int m,
                        const uint8_t in[4])
{
    int x = sbx + 2 * (m & 3), y = sby + 2 * (m >> 2);
    uint8_t *r0 = plane + (size_t)y * (size_t)W + (size_t)x;
    uint8_t *r1 = r0 + W;
    r0[0] = in[0]; r0[1] = in[1]; r1[0] = in[2]; r1[1] = in[3];
}

/* Work out which of a superblock's 16 macroblocks would change if coded (their
 * reconstruction differs from what's already on screen). Fills `codes` for all
 * 16 and returns the bit mask of the changed ones (0 = whole superblock
 * unchanged). Does not emit or mutate the reconstruction. */
static unsigned analyze_superblock(ReplayEsc122Enc *e, const uint8_t *src,
                                   int sbx, int sby, MacroCode codes[16])
{
    unsigned changed = 0;
    int m;
    for (m = 0; m < 16; m++) {
        uint8_t sc[4], rc[4];
        read_macro(src, e->W, sbx, sby, m, sc);
        codes[m] = code_block(sc, e->have_pal ? e->pal : NULL);
        read_macro(e->recon, e->W, sbx, sby, m, rc);
        if (memcmp(codes[m].recon, rc, 4) != 0) changed |= (1u << m);
    }
    return changed;
}

/* Emit a coded superblock and update the reconstruction, using both decoder
 * passes. Pass 1 "broadcasts" a single block to every macroblock that decodes to
 * the same 2x2 -- cheap for the repeated blocks of a flat region -- so we group
 * the changed macroblocks by identical reconstruction and broadcast each group of
 * two or more (one block + a 16-bit mask). The leftover singletons are cheaper to
 * send together in pass 2, which shares one mask across them. */
static void emit_superblock(ReplayEsc122Enc *e, BitW *w, int sbx, int sby,
                            const MacroCode codes[16], unsigned changed)
{
    unsigned assigned = 0;     /* macroblocks handled by a pass-1 broadcast */
    int m, n;

    /* Pass 1: one broadcast per group of >=2 identical changed macroblocks. */
    for (m = 0; m < 16; m++) {
        unsigned group;
        int count;
        if (!(changed & (1u << m)) || (assigned & (1u << m)))
            continue;
        group = (1u << m);
        count = 1;
        for (n = m + 1; n < 16; n++)
            if ((changed & (1u << n)) && !(assigned & (1u << n)) &&
                memcmp(codes[n].recon, codes[m].recon, 4) == 0) {
                group |= (1u << n);
                count++;
            }
        if (count >= 2) {
            wput(w, 0, 1);                            /* pass 1: another block */
            emit_block(w, &codes[m]);
            wput(w, group, 16);
            for (n = 0; n < 16; n++)
                if (group & (1u << n))
                    write_macro(e->recon, e->W, sbx, sby, n, codes[m].recon);
            assigned |= group;
        }
    }
    wput(w, 1, 1);                                    /* pass 1: stop */

    /* Pass 2: the remaining changed macroblocks, one block each under one mask. */
    {
        unsigned rest = changed & ~assigned;
        if (rest == 0) {
            wput(w, 1, 1);                            /* present = 1: no pass 2 */
        } else {
            wput(w, 0, 1);                            /* present = 0: pass 2 */
            wput(w, rest, 16);
            for (m = 0; m < 16; m++)
                if (rest & (1u << m)) {
                    emit_block(w, &codes[m]);
                    write_macro(e->recon, e->W, sbx, sby, m, codes[m].recon);
                }
        }
    }
}

/* ---- public API -----------------------------------------------------------*/
ReplayEsc122Enc *replay_esc122enc_open(unsigned width, unsigned height)
{
    ReplayEsc122Enc *e;
    if (width == 0 || height == 0 || (width % 8) || (height % 8))
        return NULL;
    e = calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    e->W = (int)width; e->H = (int)height;
    e->sbcols = (int)(width / 8); e->sbrows = (int)(height / 8);
    e->recon = calloc((size_t)width * height, 1);
    if (e->recon == NULL) { free(e); return NULL; }
    return e;
}

void replay_esc122enc_close(ReplayEsc122Enc *e)
{
    if (e == NULL) return;
    free(e->recon);
    free(e);
}

size_t replay_esc122enc_frame(ReplayEsc122Enc *e, const uint8_t *indices,
                              const uint8_t *palette, uint8_t *out, size_t cap)
{
    size_t pal_size, bs_start, nbytes, total;
    BitW w;
    unsigned pending = 0;
    int sbx, sby;

    if (e == NULL || indices == NULL || out == NULL)
        return 0;

    /* Chunk header: [u32 magic 0x116][u32 vsize][u16 pal_size][palette]. The
     * palette (256 entries) is emitted whenever a new one is supplied, as 6-bit
     * components (the decoder expands them 6->8); a pal_size of 0 reuses it. */
    pal_size = palette ? 768u : 0u;
    bs_start = 10u + pal_size;
    if (cap < bs_start + 4u)
        return 0;
    memset(out, 0, cap);
    out[0] = 0x16; out[1] = 0x01;                    /* 0x00000116, little-endian */
    out[8] = (uint8_t)pal_size; out[9] = (uint8_t)(pal_size >> 8);
    if (palette) {
        size_t i;
        memcpy(e->pal, palette, 768);
        e->have_pal = 1;
        for (i = 0; i < 768; i++)
            out[10 + i] = (uint8_t)(palette[i] >> 2);   /* 8-bit -> 6-bit */
    }

    /* Bitstream: for each superblock in raster order, either extend the run of
     * skipped (unchanged) superblocks, or -- for a changed one -- flush that run
     * as a skip-run VLC and then emit the coded superblock. The decoder reads the
     * skip run first, so it must precede the coded data. */
    w.buf = out + bs_start; w.cap = cap - bs_start; w.bitpos = 0; w.overflow = 0;
    for (sby = 0; sby < e->sbrows; sby++) {
        for (sbx = 0; sbx < e->sbcols; sbx++) {
            MacroCode codes[16];
            unsigned changed = analyze_superblock(e, indices, sbx * 8, sby * 8, codes);
            if (changed) {
                emit_skip_run(&w, pending);          /* run of skips before it */
                pending = 0;
                emit_superblock(e, &w, sbx * 8, sby * 8, codes, changed);
            } else {
                pending++;
            }
        }
    }
    if (pending > 0)
        emit_skip_run(&w, pending);                  /* trailing skipped run */

    if (w.overflow)
        return 0;
    nbytes = (w.bitpos + 7) / 8;
    total = bs_start + nbytes;
    out[4] = (uint8_t)total; out[5] = (uint8_t)(total >> 8);
    out[6] = (uint8_t)(total >> 16); out[7] = (uint8_t)(total >> 24);
    return total;
}

const uint8_t *replay_esc122enc_recon(const ReplayEsc122Enc *e) { return e->recon; }
