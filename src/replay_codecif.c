/* replay_codecif.c -- see include/replay/codecif.h. */

#include "replay/codecif.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standard harness memory map, matching the historical Unicorn harnesses so
 * results are directly comparable. All regions live in a flat, zero-filled,
 * allocate-on-demand address space (see replay/armsim.h). */
#define CODE_BASE      0x00100000u
#define SOURCE_BASE    0x01000000u
#define OUTPUT_BASE    0x02000000u
#define PREVIOUS_BASE  0x02800000u
#define STACK_BASE     0x03000000u
/* The return sentinel must lie below 2^26 so it survives in 26-bit mode (the
 * Replay modules return with the 26-bit `MOVS pc, lr` convention). It sits well
 * above the stack region and below the 26-bit address ceiling. */
#define RETURN_ADDRESS 0x03800000u
#define STACK_PAGE     0x00001000u

/* Region capacities used for bounds checks (mirrors the Python harness). */
#define CODE_CAPACITY   (SOURCE_BASE - CODE_BASE)
#define SOURCE_CAPACITY (OUTPUT_BASE - SOURCE_BASE)
#define FRAME_CAPACITY  (PREVIOUS_BASE - OUTPUT_BASE)

/* A generous instruction budget: a 320x256 frame decodes in a few million
 * instructions, so this only fires on a runaway payload. */
#define INSTRUCTION_BUDGET 2000000000ull

struct ReplayCodecIf {
    ReplayArmSim *sim;
    unsigned width;
    unsigned height;
    size_t pixel_count;
    size_t frame_words;   /* width*height*4 */
    uint32_t cur_out;     /* OUTPUT_BASE / PREVIOUS_BASE, swapped each frame */
    uint32_t cur_prev;
    size_t payload_len;
    int payload_loaded;
};

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (errbuf == NULL || errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Pixel-layout conversions.                                          */
/* ------------------------------------------------------------------ */

static uint32_t read_word_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_word_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t replay_pix_bytes_per_pixel(ReplayPixelLayout layout)
{
    return (layout == REPLAY_PIX_RAW16) ? 2 : 3;
}

int replay_pix_pack(ReplayPixelLayout layout, const uint8_t *bytes,
                    size_t pixel_count, uint8_t *words_out, size_t *bad_pixel)
{
    size_t i;
    for (i = 0; i < pixel_count; i++) {
        uint32_t word;
        if (layout == REPLAY_PIX_RAW16) {
            word = (uint32_t)bytes[i * 2] | ((uint32_t)bytes[i * 2 + 1] << 8);
        } else {
            unsigned y = bytes[i * 3 + 0];
            unsigned u = bytes[i * 3 + 1];
            unsigned v = bytes[i * 3 + 2];
            switch (layout) {
            case REPLAY_PIX_6Y5UV:
                if (y > 63 || u > 31 || v > 31) goto bad;
                word = y | (u << 6) | (v << 11);
                break;
            case REPLAY_PIX_YUV555:
                if (y > 31 || u > 31 || v > 31) goto bad;
                word = y | (u << 5) | (v << 10);
                break;
            case REPLAY_PIX_6Y6UV:
                if (y > 63 || u > 63 || v > 63) goto bad;
                word = y | (u << 6) | (v << 12);
                break;
            case REPLAY_PIX_YUV555_TO_6Y5UV:
            case REPLAY_PIX_RAW16:
            default:
                return -1; /* not a pack layout */
            }
        }
        write_word_le(words_out + i * 4, word);
    }
    return 0;
bad:
    if (bad_pixel != NULL)
        *bad_pixel = i;
    return -1;
}

int replay_pix_unpack(ReplayPixelLayout layout, const uint8_t *words,
                      size_t pixel_count, uint8_t *bytes_out)
{
    size_t i;
    for (i = 0; i < pixel_count; i++) {
        uint32_t w = read_word_le(words + i * 4);
        switch (layout) {
        case REPLAY_PIX_6Y5UV:
            bytes_out[i * 3 + 0] = (uint8_t)(w & 0x3F);
            bytes_out[i * 3 + 1] = (uint8_t)((w >> 6) & 0x1F);
            bytes_out[i * 3 + 2] = (uint8_t)((w >> 11) & 0x1F);
            break;
        case REPLAY_PIX_YUV555:
            bytes_out[i * 3 + 0] = (uint8_t)(w & 0x1F);
            bytes_out[i * 3 + 1] = (uint8_t)((w >> 5) & 0x1F);
            bytes_out[i * 3 + 2] = (uint8_t)((w >> 10) & 0x1F);
            break;
        case REPLAY_PIX_6Y6UV:
            bytes_out[i * 3 + 0] = (uint8_t)(w & 0x3F);
            bytes_out[i * 3 + 1] = (uint8_t)((w >> 6) & 0x3F);
            bytes_out[i * 3 + 2] = (uint8_t)((w >> 12) & 0x3F);
            break;
        case REPLAY_PIX_YUV555_TO_6Y5UV: {
            /* CompLib's full-range component conversion, rounded to nearest. */
            unsigned y5 = w & 0x1F;
            bytes_out[i * 3 + 0] = (uint8_t)((y5 * 63 + 15) / 31);
            bytes_out[i * 3 + 1] = (uint8_t)((w >> 5) & 0x1F);
            bytes_out[i * 3 + 2] = (uint8_t)((w >> 10) & 0x1F);
            break;
        }
        case REPLAY_PIX_RAW16:
            bytes_out[i * 2 + 0] = (uint8_t)(w & 0xFF);
            bytes_out[i * 2 + 1] = (uint8_t)((w >> 8) & 0xFF);
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CodecIf session.                                                   */
/* ------------------------------------------------------------------ */

ReplayCodecIf *replay_codecif_open(const uint8_t *module, size_t module_len,
                                   unsigned width, unsigned height,
                                   ReplayArmMode mode,
                                   char *errbuf, size_t errlen)
{
    ReplayCodecIf *cif;
    uint32_t ret_op;

    if (module == NULL || width == 0 || height == 0) {
        set_err(errbuf, errlen, "invalid arguments");
        return NULL;
    }
    if (module_len < 12) {
        set_err(errbuf, errlen, "module too short for its CodecIf header");
        return NULL;
    }
    if (module_len > CODE_CAPACITY) {
        set_err(errbuf, errlen, "module too large for the harness memory map");
        return NULL;
    }

    cif = calloc(1, sizeof(*cif));
    if (cif == NULL) {
        set_err(errbuf, errlen, "out of memory");
        return NULL;
    }
    cif->width = width;
    cif->height = height;
    cif->pixel_count = (size_t)width * height;
    cif->frame_words = cif->pixel_count * 4;
    cif->cur_out = OUTPUT_BASE;
    cif->cur_prev = PREVIOUS_BASE;

    if (cif->frame_words > FRAME_CAPACITY) {
        set_err(errbuf, errlen, "frame too large for the harness memory map");
        free(cif);
        return NULL;
    }

    cif->sim = replay_armsim_new(mode);
    if (cif->sim == NULL) {
        set_err(errbuf, errlen, "could not create ARM sandbox");
        free(cif);
        return NULL;
    }

    /* Lay out code, working buffers, stack and the return sentinel. */
    replay_armsim_map(cif->sim, CODE_BASE, module, module_len, module_len);
    replay_armsim_map(cif->sim, OUTPUT_BASE, NULL, 0, cif->frame_words);
    replay_armsim_map(cif->sim, PREVIOUS_BASE, NULL, 0, cif->frame_words);
    ret_op = replay_armsim_return_opcode();
    {
        uint8_t op[4];
        write_word_le(op, ret_op);
        replay_armsim_map(cif->sim, RETURN_ADDRESS, op, sizeof op, sizeof op);
    }

    /* Init entry: r0=width, r1=height, r2=r3=0 (no parameter list). */
    replay_armsim_set_reg(cif->sim, 0, width);
    replay_armsim_set_reg(cif->sim, 1, height);
    replay_armsim_set_reg(cif->sim, 2, 0);
    replay_armsim_set_reg(cif->sim, 3, 0);
    replay_armsim_set_reg(cif->sim, 13, STACK_BASE + STACK_PAGE);
    replay_armsim_set_reg(cif->sim, 14, RETURN_ADDRESS);

    if (replay_armsim_run(cif->sim, CODE_BASE + 4, INSTRUCTION_BUDGET, NULL)
        != REPLAY_ARM_OK) {
        set_err(errbuf, errlen, "decompressor init did not return cleanly");
        replay_codecif_close(cif);
        return NULL;
    }

    return cif;
}

void replay_codecif_close(ReplayCodecIf *cif)
{
    if (cif == NULL)
        return;
    replay_armsim_free(cif->sim);
    free(cif);
}

size_t replay_codecif_frame_words_len(const ReplayCodecIf *cif)
{
    return cif->frame_words;
}

int replay_codecif_load_payload(ReplayCodecIf *cif,
                                const uint8_t *payload, size_t payload_len,
                                char *errbuf, size_t errlen)
{
    if (cif == NULL || (payload == NULL && payload_len > 0)) {
        set_err(errbuf, errlen, "invalid arguments");
        return -1;
    }
    if (payload_len == 0) {
        set_err(errbuf, errlen, "payload is empty");
        return -1;
    }
    /* The decoder performs word loads a little past the final payload byte. */
    if (payload_len + 8 > SOURCE_CAPACITY) {
        set_err(errbuf, errlen, "payload too large for the harness memory map");
        return -1;
    }
    replay_armsim_map(cif->sim, SOURCE_BASE, payload, payload_len,
                      payload_len + 8);
    cif->payload_len = payload_len;
    cif->payload_loaded = 1;
    return 0;
}

int replay_codecif_set_previous_words(ReplayCodecIf *cif,
                                      const uint8_t *words, size_t len,
                                      char *errbuf, size_t errlen)
{
    if (cif == NULL || words == NULL) {
        set_err(errbuf, errlen, "invalid arguments");
        return -1;
    }
    if (len != cif->frame_words) {
        set_err(errbuf, errlen, "previous frame is %zu words bytes; expected %zu",
                len, cif->frame_words);
        return -1;
    }
    replay_armsim_write(cif->sim, cif->cur_prev, words, len);
    return 0;
}

int replay_codecif_decode(ReplayCodecIf *cif, size_t *offset,
                          uint8_t *out_words, size_t *consumed,
                          char *errbuf, size_t errlen)
{
    uint32_t next_source;
    size_t end;
    uint32_t swap;

    if (cif == NULL || offset == NULL || out_words == NULL) {
        set_err(errbuf, errlen, "invalid arguments");
        return -1;
    }
    if (!cif->payload_loaded) {
        set_err(errbuf, errlen, "no payload loaded");
        return -1;
    }
    if (*offset > cif->payload_len) {
        set_err(errbuf, errlen, "offset %zu past payload end %zu",
                *offset, cif->payload_len);
        return -1;
    }

    /* Decode entry: r0=source, r1=output, r2=previous, r3=colour-lookup (0,
     * left unpatched), r4/r14=return. */
    replay_armsim_set_reg(cif->sim, 0, SOURCE_BASE + (uint32_t)*offset);
    replay_armsim_set_reg(cif->sim, 1, cif->cur_out);
    replay_armsim_set_reg(cif->sim, 2, cif->cur_prev);
    replay_armsim_set_reg(cif->sim, 3, 0);
    replay_armsim_set_reg(cif->sim, 4, RETURN_ADDRESS);
    replay_armsim_set_reg(cif->sim, 13, STACK_BASE + STACK_PAGE);
    replay_armsim_set_reg(cif->sim, 14, RETURN_ADDRESS);

    if (replay_armsim_run(cif->sim, CODE_BASE + 8, INSTRUCTION_BUDGET, NULL)
        != REPLAY_ARM_OK) {
        set_err(errbuf, errlen, "decompressor did not return cleanly");
        return -1;
    }

    next_source = replay_armsim_get_reg(cif->sim, 0);
    if (next_source < SOURCE_BASE) {
        set_err(errbuf, errlen, "decompressor returned an invalid source pointer");
        return -1;
    }
    end = next_source - SOURCE_BASE;
    if (end < *offset || end > cif->payload_len) {
        set_err(errbuf, errlen,
                "decompressor consumed an invalid range: %zu after %zu (limit %zu)",
                end, *offset, cif->payload_len);
        return -1;
    }

    replay_armsim_read(cif->sim, cif->cur_out, out_words, cif->frame_words);

    if (consumed != NULL)
        *consumed = end - *offset;
    *offset = end;

    /* The frame just decoded is the next frame's temporal reference. */
    swap = cif->cur_out;
    cif->cur_out = cif->cur_prev;
    cif->cur_prev = swap;

    return 0;
}
