/*
 * replay_escape124.c -- Eidos/Acorn "Escape" video format 124.
 * See include/replay/replay_escape124.h.
 *
 * Reimplementation of the codec-124 algorithm documented in
 * docs/spec/eidos-escape.md (§ Type 124), reverse-engineered from WINSDEC.DLL
 * (`SC_Frame`) and cross-referenced with the publicly documented FFmpeg
 * `escape124` algorithm shape (the 0x7800000 frame-flag gate, the mask_matrix,
 * the codebook-switch transition table, the codebook-flag bits, the skip VLC).
 *
 * The ARMovie variant differs from stock escape124: the block bitstream is read
 * LSB-first, the transition-table columns are swapped, and a per-superblock
 * 17-bit "mask+continue" field and a second "pattern" placement path are added.
 * It also reproduces a real WINSDEC bit-reader quirk -- a stale dword look-ahead
 * register across 32-bit boundaries -- so the pixel output matches the shipping
 * decoder bit-for-bit. RGB555 is the codec's native pixel format (one uint16 per
 * pixel); bit 15 is set on codec-written pixels to mirror WINSDEC's 1-bit alpha.
 */
#include "replay/replay_escape124.h"

#include <stdlib.h>
#include <string.h>

/* ---- LSB-first bit reader ------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    size_t len;          /* bytes */
    size_t bitpos;       /* next bit to read */
} GetBits;

static void gb_init(GetBits *gb, const uint8_t *buf, size_t len)
{
    gb->buf = buf; gb->len = len; gb->bitpos = 0;
}
static size_t gb_left(const GetBits *gb) { return gb->len * 8 - gb->bitpos; }

/* Read n bits (0..32), LSB-first: bit 0 of a byte is least significant, and the
 * first bit read is the least-significant bit of the result. */
static uint32_t gb_get(GetBits *gb, unsigned n)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        size_t p = gb->bitpos + i;
        unsigned bit = (unsigned)((gb->buf[p >> 3] >> (p & 7)) & 1);
        v |= (uint32_t)bit << i;
    }
    gb->bitpos += n;
    return v;
}
static unsigned gb_get1(GetBits *gb) { return gb_get(gb, 1); }
/* 0-width read -> 0 (avoids the UB of a 0-bit shift). */
static uint32_t gb_getz(GetBits *gb, unsigned n) { return n ? gb_get(gb, n) : 0; }

/* Peek a 32-bit little-endian word at a 32-bit-aligned bit position, without
 * advancing -- used to emulate WINSDEC's dword bit reader (see the stale-ebx note
 * in the main loop). WINSDEC's dword boundaries fall on this stream's bitpos
 * multiples of 32 (its pointer is chunk+16 = this stream's bit 64, 64 % 32 == 0). */
static uint32_t gb_peek_dword(const GetBits *gb, size_t at)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 32; i++) {
        size_t p = at + (size_t)i;
        unsigned bit = (p < gb->len * 8) ? (unsigned)((gb->buf[p >> 3] >> (p & 7)) & 1) : 0u;
        v |= (uint32_t)bit << i;
    }
    return v;
}

static int ilog2(unsigned x) { int n = -1; while (x) { n++; x >>= 1; } return n; }

/* ---- codec data structures ------------------------------------------------ */
typedef struct { uint16_t pixels[4]; } MacroBlock;     /* 2x2 */
typedef struct { uint16_t pixels[64]; } SuperBlock;    /* 8x8 */
typedef struct { unsigned depth, size; MacroBlock *blocks; } CodeBook;

struct ReplayEsc124 {
    int width, height;          /* multiples of 8 */
    int sb_w, sb_h;             /* superblock grid */
    CodeBook cb[3];
    uint16_t *cur, *prev;       /* width*height RGB555 each */
};

/* Macroblock slot k (row-major in the 4x4 grid) -> its bit in the 16-bit mask. */
static const uint16_t mask_matrix[16] = {
    0x0001, 0x0002, 0x0010, 0x0020,
    0x0004, 0x0008, 0x0040, 0x0080,
    0x0100, 0x0200, 0x1000, 0x2000,
    0x0400, 0x0800, 0x4000, 0x8000,
};
/* Codebook-switch transitions recovered from WINSDEC (0x10007679):
 *   new = ((old + b) - 2 - borrow) & 3, tabulated as transitions[old][b].
 * The ARMovie variant's columns are swapped vs FFmpeg's {{2,1},{0,2},{1,0}}. */
static const int8_t transitions[3][2] = { {1, 2}, {2, 0}, {0, 1} };

static void unpack_codebook(ReplayEsc124 *s, GetBits *gb, int idx,
                            unsigned depth, unsigned size)
{
    CodeBook *cb = &s->cb[idx];
    unsigned i;
    /* A correct stream never exceeds (bits left)/34 entries; cap defensively so
     * corrupt / mis-framed input fails soft instead of allocating wildly. */
    unsigned maxn = (unsigned)(gb_left(gb) / 34) + 1;
    free(cb->blocks);
    if (size > maxn) size = maxn;
    cb->depth = depth; cb->size = size;
    cb->blocks = calloc(size ? size : 1, sizeof(MacroBlock));
    if (cb->blocks == NULL) { cb->size = 0; return; }
    for (i = 0; i < size && gb_left(gb) >= 34; i++) {
        unsigned mask = gb_get(gb, 4);
        uint16_t color[2];
        int j;
        /* RGB555, with bit 15 set as WINSDEC's 1-bit output alpha so decoded
         * pixels match the reference byte-for-byte (mask 0x7fff for colour). */
        color[0] = (uint16_t)(gb_get(gb, 15) | 0x8000u);
        color[1] = (uint16_t)(gb_get(gb, 15) | 0x8000u);
        for (j = 0; j < 4; j++)
            cb->blocks[i].pixels[j] = color[(mask >> j) & 1u];
    }
}

/* Escalating skip-run VLC: 1 bit, then +3, +7, +12 at the thresholds. Returns
 * (unsigned)-1 when the stream is exhausted. */
static unsigned decode_skip_count(GetBits *gb)
{
    unsigned v;
    if (gb_left(gb) < 1) return (unsigned)-1;
    v = gb_get1(gb);
    if (!v) return 0;
    v += gb_get(gb, 3);
    if (v != 1 + 7) return v;
    v += gb_get(gb, 7);
    if (v != 1 + 7 + 127) return v;
    return v + gb_get(gb, 12);
}

static MacroBlock decode_macroblock(ReplayEsc124 *s, GetBits *gb, int *cb_index,
                                    unsigned sb_index, int *idx_crossed)
{
    unsigned depth, block_index;
    if (gb_get1(gb)) {
        unsigned b = gb_get1(gb);
        *cb_index = transitions[*cb_index][b];
    }
    depth = s->cb[*cb_index].depth;
    /* Did this index read cross a 32-bit dword boundary? WINSDEC's boundary
     * handler advances its pointer without reloading the look-ahead register, so
     * the *following* mask+continue read misbehaves -- see the main loop. */
    if (idx_crossed)
        *idx_crossed = depth && ((gb->bitpos % 32) + depth) >= 32;
    block_index = gb_getz(gb, depth);
    if (*cb_index == 0)        /* the per-superblock codebook (WINSDEC ch==0) */
        block_index += sb_index << s->cb[0].depth;
    if (block_index >= s->cb[*cb_index].size) {   /* guard against corrupt data */
        MacroBlock z; memset(&z, 0, sizeof z); return z;
    }
    return s->cb[*cb_index].blocks[block_index];
}

/* Copy an 8x8 superblock between a frame plane (stride=width) and an 8-wide sb. */
static void copy_sb_from_frame(uint16_t *dst8, const uint16_t *src, int stride)
{
    int y;
    for (y = 0; y < 8; y++)
        memcpy(dst8 + y * 8, src + (size_t)y * (size_t)stride, 8 * sizeof(uint16_t));
}
static void copy_sb_to_frame(uint16_t *dst, int stride, const uint16_t *src8)
{
    int y;
    for (y = 0; y < 8; y++)
        memcpy(dst + (size_t)y * (size_t)stride, src8 + y * 8, 8 * sizeof(uint16_t));
}

/* Insert a 2x2 macroblock at slot `index` (row-major in the 4x4 grid). */
static void insert_mb_into_sb(SuperBlock *sb, MacroBlock mb, unsigned index)
{
    unsigned mr = index >> 2, mc = index & 3u;
    unsigned base = (2u * mr) * 8u + 2u * mc;   /* top-left pixel in the 8x8 */
    sb->pixels[base]      = mb.pixels[0];       /* TL */
    sb->pixels[base + 1u] = mb.pixels[1];       /* TR */
    sb->pixels[base + 8u] = mb.pixels[2];       /* BL */
    sb->pixels[base + 9u] = mb.pixels[3];       /* BR */
}

/* Pattern path: a 4-bit selector chooses one of 16 "compact mask" handlers
 * (WINSDEC jump table @0x100085b9); each reads a few more bits and expands them
 * into a 16-bit mask, forcing certain nibbles to 0xf. Returns that mask (the
 * caller XORs it with the accumulated mask). The bit juggling mirrors WINSDEC's
 * al/ah byte handling; only the low 16 bits matter for placement. */
static uint32_t pattern_delta(GetBits *gb, unsigned sel)
{
    uint32_t e, ah;
    switch (sel & 0xf) {
    case 0:  return gb_get(gb, 16);
    case 1:  e = gb_get(gb, 12); return (e << 4) | 0xf;
    case 2:  e = gb_get(gb, 12); e <<= 4;
             ah = (e & 0xff); ah = (ah >> 4) | 0xf0;
             return (e & ~0xffu) | ah;
    case 3:  e = gb_get(gb, 8);  return ((e & 0xff) << 8) | 0xff;
    case 4:  e = gb_get(gb, 12);
             ah = (e >> 8) & 0xff; ah = ((ah << 4) | 0xf) & 0xff;
             return (e & ~0xff00u) | (ah << 8);
    case 5:  e = gb_get(gb, 8);  e <<= 4;
             ah = (e >> 8) & 0xff; ah = (ah << 4) & 0xff;
             return ((e & ~0xff00u) | (ah << 8)) | 0xf0f;
    case 6:  e = gb_get(gb, 8);  ah = e & 0xff;
             return ((e & ~0xff00u) | (ah << 8)) | 0xff0;
    case 7:  e = gb_get(gb, 4);  return (e << 12) | 0xfff;
    case 8:  e = gb_get(gb, 12);
             ah = (e >> 8) & 0xff; ah |= 0xf0;
             return (e & ~0xff00u) | (ah << 8);
    case 9:  e = gb_get(gb, 8);  return (e << 4) | 0xf00f;
    case 10: e = gb_get(gb, 8);  ah = e & 0xff;
             ah = ((ah >> 4) | (ah << 4)) & 0xff;          /* ror ah,4 */
             return ((e & ~0xff00u) | (ah << 8)) | 0xf0f0;
    case 11: e = gb_get(gb, 4);  ah = e & 0xff;
             return ((e & ~0xff00u) | (ah << 8)) | 0xf0ff;
    case 12: e = gb_get(gb, 8);  return (e & ~0xff00u) | 0xff00;
    case 13: e = gb_get(gb, 4);  e |= 0xfff0;              /* then rol al,4 */
             ah = e & 0xff; ah = ((ah << 4) | (ah >> 4)) & 0xff;
             return (e & ~0xffu) | ah;
    case 14: e = gb_get(gb, 4);  return e | 0xfff0;
    default: return 0xffff;
    }
}

/* ---- public API ----------------------------------------------------------- */
ReplayEsc124 *replay_esc124_open(unsigned width, unsigned height)
{
    ReplayEsc124 *s;
    if (width == 0 || height == 0 || (width % 8) || (height % 8))
        return NULL;
    s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->width = (int)width; s->height = (int)height;
    s->sb_w = (int)(width / 8); s->sb_h = (int)(height / 8);
    s->cur = calloc((size_t)width * height, sizeof(uint16_t));
    s->prev = calloc((size_t)width * height, sizeof(uint16_t));
    if (s->cur == NULL || s->prev == NULL) {
        replay_esc124_close(s);
        return NULL;
    }
    return s;
}

void replay_esc124_close(ReplayEsc124 *s)
{
    int i;
    if (s == NULL)
        return;
    for (i = 0; i < 3; i++)
        free(s->cb[i].blocks);
    free(s->cur);
    free(s->prev);
    free(s);
}

int replay_esc124_decode(ReplayEsc124 *s, const uint8_t *frame, size_t len)
{
    GetBits gb;
    uint32_t flags;
    uint16_t *tmp;
    unsigned num_sb, sbi;
    int i, cb_index, skip;

    if (s == NULL || frame == NULL || len < 8)
        return -1;

    /* This frame starts from the previous one; skipped superblocks persist.
     * Swap so `prev` holds the last output, then decode into `cur`. */
    tmp = s->prev; s->prev = s->cur; s->cur = tmp;
    memcpy(s->cur, s->prev, (size_t)s->width * (size_t)s->height * sizeof(uint16_t));

    gb_init(&gb, frame, len);
    flags = gb_get(&gb, 32);
    (void)gb_get(&gb, 32);                 /* size (unused here) */

    /* SC_Frame gates only on 0x7800000 (FFmpeg's extra 0x114 term is not in the
     * DLL). A gated-out frame is a verbatim copy of the previous one. */
    if (!(flags & 0x7800000u))
        return 0;

    num_sb = (unsigned)s->sb_w * (unsigned)s->sb_h;

    /* Unpack up to three codebooks in flag-bit order 17,18,19. The stored slot is
     * not the flag order:
     *   bit 17 -> slot 1, size = 1<<depth
     *   bit 18 -> slot 0, size = num_sb<<depth  (the per-superblock codebook)
     *   bit 19 -> slot 2, size = get(20), depth = ilog2(size-1)+1 */
    for (i = 0; i < 3; i++) {
        unsigned depth, size;
        int slot;
        if (!(flags & (1u << (17 + i)))) continue;
        if (i == 0)      { slot = 1; depth = gb_get(&gb, 4); size = 1u << depth; }
        else if (i == 1) { slot = 0; depth = gb_get(&gb, 4); size = num_sb << depth; }
        else             { slot = 2; size = gb_get(&gb, 20);
                           depth = (unsigned)ilog2(size - 1) + 1; }
        unpack_codebook(s, &gb, slot, depth, size);
    }

    cb_index = 0;              /* WINSDEC inits ch=0 (not FFmpeg's 1) */
    skip = -1;

    for (sbi = 0; sbi < num_sb; sbi++) {
        int sx = (int)(sbi % (unsigned)s->sb_w) * 8;
        int sy = (int)(sbi / (unsigned)s->sb_w) * 8;
        size_t foff = (size_t)sy * (size_t)s->width + (size_t)sx;
        uint16_t *fdst = s->cur + foff;
        const uint16_t *fsrc = s->prev + foff;
        SuperBlock sb;

        if (skip == -1) skip = (int)decode_skip_count(&gb);
        if (skip < 0) break;

        if (skip) {
            /* superblock copied verbatim from previous frame (already present) */
        } else if (gb_left(&gb) >= 1) {
            unsigned accumulated = 0;
            int do_pattern;
            copy_sb_from_frame(sb.pixels, fsrc, s->width);

            do_pattern = (int)gb_get1(&gb);   /* leading bit: 1 -> straight to pattern */

            if (!do_pattern) {
                /* main loop: one MB -> every set slot in a 16-bit mask; a 17th
                 * "continue" bit (==1) drops out into the pattern path. */
                for (;;) {
                    int idx_crossed = 0;
                    MacroBlock mb = decode_macroblock(s, &gb, &cb_index, sbi,
                                                      &idx_crossed);
                    unsigned mask, cont;
                    int k;
                    /* When the preceding index read crossed a dword boundary,
                     * WINSDEC's reader left its look-ahead register stale, so this
                     * 17-bit read rotates the current dword instead of pulling the
                     * high bits from the next one. Reproduce that when the field
                     * spans into the next dword (start offset >= 16). */
                    if (idx_crossed && (gb.bitpos % 32) + 17 > 32) {
                        unsigned sh = (unsigned)(gb.bitpos % 32);
                        uint32_t dw = gb_peek_dword(&gb, gb.bitpos - sh);
                        uint32_t v = ((dw >> sh) | (dw << (32 - sh))) & 0x1ffffu;
                        mask = v & 0xffffu; cont = (v >> 16) & 1u;
                        gb.bitpos += 17;
                    } else {
                        mask = gb_get(&gb, 16);
                        cont = gb_get1(&gb);
                    }
                    accumulated |= mask;
                    for (k = 0; k < 16; k++)
                        if (mask & mask_matrix[k])
                            insert_mb_into_sb(&sb, mb, (unsigned)k);
                    if (cont) { do_pattern = 1; break; }
                }
            }

            if (do_pattern) {
                unsigned sub = gb_get1(&gb);
                if (sub) {
                    /* sub-mode: explicit 4-bit slot per MB (gated on flags&0x10000) */
                    if (flags & 0x10000u) {
                        while (!gb_get1(&gb)) {
                            MacroBlock mb = decode_macroblock(s, &gb, &cb_index,
                                                              sbi, NULL);
                            unsigned pos = gb_get(&gb, 4);
                            insert_mb_into_sb(&sb, mb, pos);
                        }
                    }
                } else {
                    /* compact-mask selector -> per-slot distinct MBs */
                    unsigned sel = gb_get(&gb, 4);
                    unsigned ebp = accumulated ^ pattern_delta(&gb, sel);
                    int k;
                    for (k = 0; k < 16; k++)
                        if (ebp & mask_matrix[k]) {
                            MacroBlock mb = decode_macroblock(s, &gb, &cb_index,
                                                              sbi, NULL);
                            insert_mb_into_sb(&sb, mb, (unsigned)k);
                        }
                }
            }
            copy_sb_to_frame(fdst, s->width, sb.pixels);
        } else break;
        skip--;
    }
    return 0;
}

const uint16_t *replay_esc124_frame(const ReplayEsc124 *s) { return s->cur; }
