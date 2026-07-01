/*
 * replay_escape100.c -- Eidos/Acorn "Escape" video formats 100 and 102.
 * See include/replay/replay_escape100.h.
 *
 * A native clean C reimplementation, reverse-engineered from the vendored
 * Decomp100 / Decomp102 ARM modules (docs/spec/eidos-escape.md § Type 100/102).
 * This describes the *format*, not the ARM code: no register iteration, just the
 * bitstream. Validated byte-exact against those modules (the golden decoders) --
 * on the SplashBox sample for 100, and on differential fuzzing for both.
 *
 * Pixels are 15-bit YUV555 words: Y in bits 0..4, a combined U/V chroma pair in
 * bits 5..14. The picture is an 80x64 grid of 2x2-pixel blocks and persists
 * across frames (delta-coded).
 */
#include "replay/replay_escape100.h"

#include <stdlib.h>
#include <string.h>

#define YMASK 0x1Fu    /* the luma (Y) bits of a YUV555 pixel */

/* 256-entry chroma codebook, extracted verbatim from the Decomp100/102 modules
 * (identical in both). Each entry is a YUV555 value with Y=0 -- the U/V pair a
 * chroma index selects; a pixel is `codebook[index] | luma5`. */
static const uint16_t CHROMA[256] = {
    0x77a0, 0x77c0, 0x77e0, 0x7400, 0x7420, 0x7440, 0x7460, 0x7ba0,
    0x7bc0, 0x7be0, 0x7800, 0x7820, 0x7840, 0x7860, 0x7fa0, 0x7fc0,
    0x7fe0, 0x7c00, 0x7c20, 0x7c40, 0x7c60, 0x03a0, 0x03c0, 0x03e0,
    0x0000, 0x0020, 0x0040, 0x0060, 0x07a0, 0x07c0, 0x07e0, 0x0400,
    0x0420, 0x0440, 0x0460, 0x0ba0, 0x0bc0, 0x0be0, 0x0800, 0x0820,
    0x0840, 0x0860, 0x0fa0, 0x0fc0, 0x0fe0, 0x0c00, 0x0c20, 0x0c40,
    0x0c60, 0x3340, 0x30c0, 0x2400, 0x1a80, 0x1800, 0x1980, 0x0cc0,
    0x02e0, 0x0180, 0x74c0, 0x6ae0, 0x6860, 0x5ee0, 0x5c60, 0x53a0,
    0x77a0, 0x77c0, 0x77e0, 0x7400, 0x7420, 0x7440, 0x7460, 0x7ba0,
    0x7bc0, 0x7be0, 0x7800, 0x7820, 0x7840, 0x7860, 0x7fa0, 0x7fc0,
    0x7fe0, 0x7c00, 0x7c20, 0x7c40, 0x7c60, 0x03a0, 0x03c0, 0x03e0,
    0x0000, 0x0020, 0x0040, 0x0060, 0x07a0, 0x07c0, 0x07e0, 0x0400,
    0x0420, 0x0440, 0x0460, 0x0ba0, 0x0bc0, 0x0be0, 0x0800, 0x0820,
    0x0840, 0x0860, 0x0fa0, 0x0fc0, 0x0fe0, 0x0c00, 0x0c20, 0x0c40,
    0x0c60, 0x33a0, 0x26e0, 0x2460, 0x1ae0, 0x1860, 0x0e80, 0x0d20,
    0x0340, 0x7680, 0x7520, 0x6b40, 0x68c0, 0x5f40, 0x5cc0, 0x5000,
    0x77a0, 0x77c0, 0x77e0, 0x7400, 0x7420, 0x7440, 0x7460, 0x7ba0,
    0x7bc0, 0x7be0, 0x7800, 0x7820, 0x7840, 0x7860, 0x7fa0, 0x7fc0,
    0x7fe0, 0x7c00, 0x7c20, 0x7c40, 0x7c60, 0x03a0, 0x03c0, 0x03e0,
    0x0000, 0x0020, 0x0040, 0x0060, 0x07a0, 0x07c0, 0x07e0, 0x0400,
    0x0420, 0x0440, 0x0460, 0x0ba0, 0x0bc0, 0x0be0, 0x0800, 0x0820,
    0x0840, 0x0860, 0x0fa0, 0x0fc0, 0x0fe0, 0x0c00, 0x0c20, 0x0c40,
    0x0c60, 0x3000, 0x2740, 0x24c0, 0x1b40, 0x18c0, 0x0ee0, 0x0d80,
    0x00c0, 0x76e0, 0x7580, 0x6ba0, 0x6920, 0x5fa0, 0x5d20, 0x5060,
    0x77a0, 0x77c0, 0x77e0, 0x7400, 0x7420, 0x7440, 0x7460, 0x7ba0,
    0x7bc0, 0x7be0, 0x7800, 0x7820, 0x7840, 0x7860, 0x7fa0, 0x7fc0,
    0x7fe0, 0x7c00, 0x7c20, 0x7c40, 0x7c60, 0x03a0, 0x03c0, 0x03e0,
    0x0000, 0x0020, 0x0040, 0x0060, 0x07a0, 0x07c0, 0x07e0, 0x0400,
    0x0420, 0x0440, 0x0460, 0x0ba0, 0x0bc0, 0x0be0, 0x0800, 0x0820,
    0x0840, 0x0860, 0x0fa0, 0x0fc0, 0x0fe0, 0x0c00, 0x0c20, 0x0c40,
    0x0c60, 0x33a0, 0x27a0, 0x2520, 0x1ba0, 0x1920, 0x0f40, 0x0280,
    0x0120, 0x7740, 0x6a80, 0x6800, 0x6980, 0x5c00, 0x5340, 0x50c0,
};

/* ---- bit reader: LSB-first, 32-bit little-endian words (as the modules read) --
 * A frame is decoded a 32-bit word at a time; the first bit consumed is the least
 * significant bit of the first word. Reads past the end return 0. Frames are
 * word-aligned: `wordpos` (bytes) is where the next frame begins. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t wordpos;    /* byte offset of the next word to load */
    uint32_t acc;      /* current bits, LSB = next to read */
    int navail;        /* valid bits remaining in acc (0..32) */
} BitR;

static uint32_t load_word(BitR *b)
{
    uint32_t w = 0;
    int i;
    for (i = 0; i < 4; i++)
        if (b->wordpos + (size_t)i < b->len)
            w |= (uint32_t)b->data[b->wordpos + (size_t)i] << (8 * i);
    b->wordpos += 4;
    return w;
}

static void br_init(BitR *b, const uint8_t *data, size_t len)
{
    b->data = data; b->len = len; b->wordpos = 0;
    b->acc = load_word(b); b->navail = 32;
}

/* Read n bits (1..16), LSB-first, spanning a word boundary if needed. */
static unsigned rd(BitR *b, int n)
{
    unsigned v;
    if (n <= b->navail) {
        v = b->acc & ((1u << n) - 1u);
        b->acc >>= n; b->navail -= n;
    } else {
        int got = b->navail;
        v = b->acc & ((1u << got) - 1u);
        b->acc = load_word(b);
        {
            int need = n - got;
            v |= (b->acc & ((1u << need) - 1u)) << got;
            b->acc >>= need; b->navail = 32 - need;
        }
    }
    /* The modules reload eagerly the moment a word is exhausted (their bit
     * counter hits 32), so a frame that ends on a word boundary has already
     * stepped the source one word further. Match it, so the consumed length
     * (hence the next frame's start) agrees exactly. */
    if (b->navail == 0) { b->acc = load_word(b); b->navail = 32; }
    return v;
}
static unsigned rd1(BitR *b) { return rd(b, 1); }

/* ---- variable-length fields -------------------------------------------------*/

/* Skip-run VLC: how many blocks to copy from the previous frame. 1 bit for 0,
 * then an escalating 3/7/15-bit tail at each all-ones threshold. */
static unsigned read_skip(BitR *b)
{
    unsigned v;
    if (rd1(b) == 0) return 0;
    v = rd(b, 3);  if (v != 7)   return 1 + v;
    v = rd(b, 7);  if (v != 127) return 8 + v;
    v = rd(b, 15); return 135 + v;
}

/* Chroma codebook index: 6 bits for 0..48, otherwise 2 more bits (top part). */
static unsigned read_chroma_index(BitR *b)
{
    unsigned i = rd(b, 6);
    if (i > 48) i += rd(b, 2) << 6;
    return i;
}

struct ReplayEsc100 {
    int w, h;
    uint16_t *pix;     /* w*h YUV555 picture, persistent across frames */
};

/* ---- public API -------------------------------------------------------------*/
ReplayEsc100 *replay_esc100_open(unsigned width, unsigned height)
{
    ReplayEsc100 *s;
    if (width == 0 || height == 0 || (width % 2) || (height % 2))
        return NULL;
    s = calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    s->w = (int)width; s->h = (int)height;
    s->pix = calloc((size_t)width * height, sizeof(uint16_t));
    if (s->pix == NULL) { free(s); return NULL; }
    return s;
}

void replay_esc100_close(ReplayEsc100 *s)
{
    if (s == NULL) return;
    free(s->pix);
    free(s);
}

/* Write one 2x2 block. The 3-bit selector `sel` chooses, per non-top-left
 * sub-pixel, luma B over luma A (bit0->TR, bit1->BL, bit2->BR); TL is always A.
 * All four share `chroma`. */
static void put_block(ReplayEsc100 *s, int bx, int by,
                      uint16_t chroma, unsigned lumaA, unsigned lumaB, unsigned sel)
{
    size_t tl = (size_t)(by * 2) * (size_t)s->w + (size_t)(bx * 2);
    size_t bl = tl + (size_t)s->w;
    uint16_t a = (uint16_t)(chroma | lumaA);
    uint16_t bpix = (uint16_t)(chroma | lumaB);
    s->pix[tl]     = a;
    s->pix[tl + 1] = (sel & 1) ? bpix : a;
    s->pix[bl]     = (sel & 2) ? bpix : a;
    s->pix[bl + 1] = (sel & 4) ? bpix : a;
}

size_t replay_esc100_decode(ReplayEsc100 *s, const uint8_t *frame, size_t len)
{
    uint32_t id;
    size_t hdr;
    BitR b;
    int bx = 0, by = 0;
    int bw = s ? s->w / 2 : 0;    /* block grid: 80x64 for the modules' 160x128 */
    int bh = s ? s->h / 2 : 0;

    if (s == NULL || frame == NULL || len < 4)
        return 0;
    id = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8)
       | ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24);
    if (id == 0x100) hdr = 4;             /* [id]                    */
    else if (id == 0x102) hdr = 8;        /* [id][reserved word]     */
    else return 0;
    if (len < hdr) return 0;

    br_init(&b, frame + hdr, len - hdr);

    for (;;) {
        /* Advance the block cursor past a skipped run (blocks kept from the
         * previous frame), wrapping rows at BW; stop after the last row. */
        /* A skip run advances the cursor, wrapping rows; a run past the last row
         * ends the frame. (The ARM modules wrap with a signed compare that
         * mishandles a pathological >=32768 run -- impossible in a real
         * 5120-block frame -- so this wraps sanely instead.) */
        bx += (int)read_skip(&b);
        while (bx >= bw) { bx -= bw; by++; }
        if (by >= bh) break;

        if (rd1(&b) == 1) {
            /* Luma block: selector (3) + luma A (5); luma B (5) unless the
             * selector is 0; then a "new chroma" flag. */
            unsigned sel   = rd(&b, 3);
            unsigned lumaA = rd(&b, 5);
            unsigned lumaB = sel ? rd(&b, 5) : 0;
            uint16_t chroma;
            if (rd1(&b) == 0) {
                /* keep the block's existing chroma (from its top-left pixel) */
                size_t tl = (size_t)(by * 2) * (size_t)s->w + (size_t)(bx * 2);
                chroma = (uint16_t)(s->pix[tl] & ~YMASK);
            } else {
                chroma = CHROMA[read_chroma_index(&b)];
            }
            put_block(s, bx, by, chroma, lumaA, lumaB, sel);
        } else {
            /* Chroma block: new chroma only; each pixel keeps its own luma. */
            uint16_t chroma = CHROMA[read_chroma_index(&b)];
            size_t tl = (size_t)(by * 2) * (size_t)s->w + (size_t)(bx * 2);
            size_t bl = tl + (size_t)s->w;
            s->pix[tl]     = (uint16_t)(chroma | (s->pix[tl]     & YMASK));
            s->pix[tl + 1] = (uint16_t)(chroma | (s->pix[tl + 1] & YMASK));
            s->pix[bl]     = (uint16_t)(chroma | (s->pix[bl]     & YMASK));
            s->pix[bl + 1] = (uint16_t)(chroma | (s->pix[bl + 1] & YMASK));
        }
        bx++;   /* advance to the next block */
    }
    return hdr + b.wordpos;
}

const uint16_t *replay_esc100_frame(const ReplayEsc100 *s) { return s->pix; }
