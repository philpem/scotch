/*
 * Differential test: the native replay_esc100 decoder vs the vendored Decomp100 /
 * Decomp102 ARM modules (the golden decoders), on synthetic frames that exercise
 * every path through the format -- both block modes, all 8 luma selectors, the
 * skip-run VLC tiers, 6-bit and 8-bit chroma indices, and the keep-chroma /
 * keep-luma delta paths. A small reference encoder emits the frames; the module
 * (run under the ARMulator) and the native decoder must produce byte-identical
 * YUV555 pictures and agree on the frame length.
 *
 * argv[1] = path to a Decompress,ffd module; argv[2] = codec id (256 or 258).
 * Exits 77 (skip) if the module is not present.
 */
#include "replay/replay_escape100.h"
#include "replay/codecif.h"
#include "replay/armsim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 160
#define H 128
#define NBLOCKS ((W / 2) * (H / 2))   /* 5120 */

/* ---- LSB-first bit writer (matches the decoder's reader) -------------------*/
static uint8_t g_buf[1 << 16];
static size_t g_bitpos;
static void wr(uint32_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1u) g_buf[g_bitpos >> 3] |= (uint8_t)(1u << (g_bitpos & 7));
        g_bitpos++;
    }
}

/* Encode the format's variable-length fields (inverse of the decoder). */
static void emit_skip(unsigned run)
{
    if (run == 0) { wr(0, 1); return; }
    wr(1, 1);
    if (run <= 7)   { wr(run - 1, 3); return; }
    wr(7, 3);
    if (run <= 134) { wr(run - 8, 7); return; }
    wr(127, 7);
    wr(run - 135, 15);
}
static void emit_chroma_index(unsigned idx)
{
    if (idx <= 48) { wr(idx, 6); return; }
    wr(idx & 63u, 6);          /* low 6 bits must be in 49..63 for a valid code */
    wr(idx >> 6, 2);
}
static void emit_luma(unsigned sel, unsigned lumaA, unsigned lumaB,
                      int newchroma, unsigned chroma_idx)
{
    wr(1, 1);                  /* mode = luma */
    wr(sel, 3);
    wr(lumaA, 5);
    if (sel != 0) wr(lumaB, 5);
    wr((unsigned)newchroma, 1);
    if (newchroma) emit_chroma_index(chroma_idx);
}
static void emit_chroma(unsigned chroma_idx)
{
    wr(0, 1);                  /* mode = chroma-only */
    emit_chroma_index(chroma_idx);
}

/* Begin a frame's byte buffer with the codec header; return the header size. */
static size_t begin_frame(uint8_t *out, unsigned id)
{
    memset(g_buf, 0, sizeof g_buf); g_bitpos = 0;
    out[0] = (uint8_t)id; out[1] = (uint8_t)(id >> 8); out[2] = 0; out[3] = 0;
    if (id == 0x102) { memset(out + 4, 0, 4); return 8; }
    return 4;
}
/* Finish: append the bitstream after the header; return total byte length. */
static size_t end_frame(uint8_t *out, size_t hdr)
{
    size_t nbytes = (g_bitpos + 7) / 8;
    /* pad to a word boundary so the module's word reads stay in-bounds */
    while (nbytes % 4) nbytes++;
    memcpy(out + hdr, g_buf, nbytes);
    return hdr + nbytes;
}

/* Compare the module and native decoder on one frame; 0 = identical. */
static int check(ReplayCodecIf *cif, ReplayEsc100 *mine,
                 const uint8_t *frame, size_t flen)
{
    char err[256];
    size_t off = 0, consumed = 0, myc, i;
    static uint8_t ow[W * H * 4];
    const uint16_t *mp;

    if (replay_codecif_load_payload(cif, frame, flen, err, sizeof err) != 0) {
        fprintf(stderr, "load: %s\n", err); return 1;
    }
    if (replay_codecif_decode(cif, &off, ow, &consumed, err, sizeof err) != 0) {
        fprintf(stderr, "module decode: %s\n", err); return 1;
    }
    myc = replay_esc100_decode(mine, frame, flen);
    mp = replay_esc100_frame(mine);
    for (i = 0; i < (size_t)W * H; i++) {
        uint32_t w = (uint32_t)ow[i * 4] | ((uint32_t)ow[i * 4 + 1] << 8)
                   | ((uint32_t)ow[i * 4 + 2] << 16) | ((uint32_t)ow[i * 4 + 3] << 24);
        if (mp[i] != (uint16_t)(w & 0xFFFFu)) {
            fprintf(stderr, "pixel %zu: native=%04x module=%04x\n",
                    i, mp[i], (uint16_t)(w & 0xFFFFu));
            return 1;
        }
    }
    if (myc != consumed) {
        fprintf(stderr, "length: native=%zu module=%zu\n", myc, consumed);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    uint8_t *mod;
    size_t mlen;
    unsigned id;
    static uint8_t frame[1 << 16];
    size_t hdr, flen;
    char err[256];
    ReplayCodecIf *cif;
    ReplayEsc100 *mine;
    int rc = 0, blk;

    if (argc < 3) { fprintf(stderr, "usage: %s module id\n", argv[0]); return 2; }
    id = (unsigned)strtoul(argv[2], NULL, 0);

    f = fopen(argv[1], "rb");
    if (f == NULL) { fprintf(stderr, "module %s absent; skipping\n", argv[1]); return 77; }
    fseek(f, 0, SEEK_END); { long s = ftell(f); mlen = (size_t)s; } fseek(f, 0, SEEK_SET);
    mod = malloc(mlen);
    if (fread(mod, 1, mlen, f) != mlen) { fclose(f); return 2; }
    fclose(f);

    cif = replay_codecif_open(mod, mlen, W, H, REPLAY_ARM_MODE_26, err, sizeof err);
    mine = replay_esc100_open(W, H);
    if (cif == NULL || mine == NULL) { fprintf(stderr, "open: %s\n", err); return 2; }

    /* Frame 1 (intra): every 4th block is a luma block cycling the 8 selectors and
     * both chroma-index widths; the rest are chroma-only blocks. */
    hdr = begin_frame(frame, id);
    emit_skip(0);
    for (blk = 0; blk < 300; blk++) {
        if (blk % 4 == 0) {
            unsigned sel = (unsigned)(blk / 4) & 7u;
            unsigned idx = (blk & 1) ? (unsigned)(49 + (blk % 15)) /* 8-bit */
                                     : (unsigned)(blk % 49);       /* 6-bit */
            emit_luma(sel, (unsigned)(blk & 31), (unsigned)((blk * 3) & 31), 1, idx);
        } else {
            emit_chroma((unsigned)(blk % 49));
        }
        if (blk != 299) emit_skip(0);
    }
    emit_skip((unsigned)(NBLOCKS - 300));   /* run to the end */
    flen = end_frame(frame, hdr);
    if (check(cif, mine, frame, flen)) { fprintf(stderr, "intra frame FAILED\n"); rc = 1; }

    /* Frame 2 (delta): skip runs of every VLC tier, keep-chroma luma blocks
     * (new-chroma flag 0), and chroma-only blocks -- all against frame 1's data. */
    hdr = begin_frame(frame, id);
    emit_skip(3);            /* small tier */
    emit_luma(5, 7, 20, 0 /* keep chroma */, 0);
    emit_skip(40);           /* mid tier */
    emit_chroma(60);         /* 8-bit index, keep luma */
    emit_skip(200);          /* large tier */
    emit_luma(2, 15, 3, 1, 12);
    {
        /* advance cursor to end: 3+1 +40+1 +200+1 +1 = 247 blocks used so far */
        emit_skip((unsigned)(NBLOCKS - 247));
    }
    flen = end_frame(frame, hdr);
    if (check(cif, mine, frame, flen)) { fprintf(stderr, "delta frame FAILED\n"); rc = 1; }

    replay_codecif_close(cif);
    replay_esc100_close(mine);
    free(mod);
    if (rc == 0) printf("OK (%s id %u)\n", argv[1], id);
    return rc;
}
