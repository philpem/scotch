#include "replay/codec_movinglines.h"

#include <stddef.h>

/*
 * Moving Lines (compression type 1). See docs/spec/type1-moving-lines.md for the
 * authoritative format; this file is its reference codec.
 *
 * Command codes (code = word >> 7, the 9-bit field above the move flag):
 *   word & 1 == 0          one literal 15-bit pixel (word >> 1)
 *   code 0x000..0x11f      temporal copy, length ((word>>1)&0x3f)+2
 *   code 0x120..0x1cb      spatial copy,  length ((word>>1)&0x3f)+2
 *   code 0x1cc, len 0      end of frame
 *   code 0x1cc, len != 0   repeated pixel, count len+2, next halfword is the pixel
 *   word>>11 == 0x1e       same-position previous-frame copy, length ((word>>1)&0x3ff)+1
 *   word>>11 == 0x1f       long literal run, length ((word>>1)&0x3ff)+1, bit-packed pixels
 *
 * The copy offset tables follow Acorn's BatchComp construction: temporal codes
 * index a -8..+8 grid in raster order with the centre (same position) omitted;
 * spatial codes 0x120+ index rows -9..-1 by columns -9..+9.
 */

#define ML_FLAG 1U
#define ML_EOF_WORD (1U + (0x1CCU << 7))

/* Temporal copy offset (in pixels) for code 0x000..0x11f: the -8..+8 grid in
   raster order, with the centre (0,0) entry skipped. */
static long temporal_offset(unsigned code, unsigned width)
{
    unsigned grid = code < 144U ? code : code + 1U; /* re-insert skipped centre */
    int dy = (int)(grid / 17U) - 8;
    int dx = (int)(grid % 17U) - 8;

    return (long)dy * (long)width + dx;
}

/* Spatial copy offset (in pixels) for code 0x120..0x1ca: rows -9..-1 (always
   already reconstructed) by columns -9..+9, in raster order. */
static long spatial_offset(unsigned code, unsigned width)
{
    unsigned k = code - 0x120U; /* 0..170 */
    int dy = (int)(k / 19U) - 9;
    int dx = (int)(k % 19U) - 9;

    return (long)dy * (long)width + dx;
}

/* Read `count` bits (<=15) least-significant-bit first from `bitpos`. Returns 0
   if the read would run past `size`. */
static int read_bits_lsb(const uint8_t *data, size_t size, size_t bitpos,
                         unsigned count, uint32_t *value)
{
    uint32_t result = 0U;
    unsigned i;

    for (i = 0U; i < count; ++i) {
        size_t byte = (bitpos + i) >> 3;

        if (byte >= size) {
            return 0;
        }
        result |= (uint32_t)((data[byte] >> ((bitpos + i) & 7U)) & 1U) << i;
    }
    *value = result;
    return 1;
}

/* Copy `len` pixels into decoded[p..] from `source[p + offset ..]`, checking
   that both ends stay inside the frame. Returns 0 on an out-of-range reference. */
static int copy_run(uint16_t *decoded, const uint16_t *source, size_t p,
                    unsigned len, long offset, size_t total)
{
    unsigned i;

    if (source == NULL || (size_t)len > total - p) {
        return 0;
    }
    for (i = 0U; i < len; ++i) {
        long src = (long)(p + i) + offset;

        if (src < 0 || (size_t)src >= total) {
            return 0;
        }
        decoded[p + i] = source[src];
    }
    return 1;
}

ReplayStatus codec_movinglines_decode_frame(const uint8_t *payload,
                                            size_t payload_size,
                                            const uint16_t *previous,
                                            uint16_t *decoded, unsigned width,
                                            unsigned height, size_t *consumed)
{
    size_t total;
    size_t pos = 0U;
    size_t p = 0U;

    if (decoded == NULL || (payload == NULL && payload_size != 0U) ||
        width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        return REPLAY_INVALID_ARGUMENT;
    }
    total = (size_t)width * height;

    for (;;) {
        unsigned w;
        unsigned code;

        if (pos + 2U > payload_size) {
            return REPLAY_TRUNCATED_INPUT;
        }
        w = (unsigned)payload[pos] | ((unsigned)payload[pos + 1U] << 8U);
        pos += 2U;

        if ((w & ML_FLAG) == 0U) {
            /* Literal 15-bit pixel. */
            if (p >= total) {
                return REPLAY_MALFORMED_STREAM;
            }
            decoded[p++] = (uint16_t)((w >> 1U) & 0x7FFFU);
            continue;
        }

        code = w >> 7U;
        if (code < 0x120U) {
            /* Temporal copy from the previous frame. */
            unsigned len = ((w >> 1U) & 0x3FU) + 2U;

            if (!copy_run(decoded, previous, p, len,
                          temporal_offset(code, width), total)) {
                return REPLAY_MALFORMED_STREAM;
            }
            p += len;
        } else if (code < 0x1CCU) {
            /* Spatial copy from the current frame (earlier rows). */
            unsigned len = ((w >> 1U) & 0x3FU) + 2U;

            if (!copy_run(decoded, decoded, p, len,
                          spatial_offset(code, width), total)) {
                return REPLAY_MALFORMED_STREAM;
            }
            p += len;
        } else if (code < 0x1E0U) {
            /* Repeated pixel, or end of frame (code 0x1cc, length 0). Codes
               0x1cd..0x1df are tolerated repeat aliases. */
            unsigned count = ((w >> 1U) & 0x3FU);
            unsigned pixel_word;

            if (code == 0x1CCU && count == 0U) {
                break; /* end of frame */
            }
            count += 2U;
            if (pos + 2U > payload_size) {
                return REPLAY_TRUNCATED_INPUT;
            }
            pixel_word = (unsigned)payload[pos] |
                         ((unsigned)payload[pos + 1U] << 8U);
            pos += 2U;
            if ((size_t)count > total - p) {
                return REPLAY_MALFORMED_STREAM;
            }
            while (count-- != 0U) {
                decoded[p++] = (uint16_t)(pixel_word & 0x7FFFU);
            }
        } else if ((w >> 11U) == 0x1EU) {
            /* Same-position copy from the previous frame (the omitted centre). */
            unsigned len = ((w >> 1U) & 0x3FFU) + 1U;

            if (!copy_run(decoded, previous, p, len, 0L, total)) {
                return REPLAY_MALFORMED_STREAM;
            }
            p += len;
        } else {
            /* (w >> 11) == 0x1f: long literal run of bit-packed 15-bit pixels,
               then realign to the next halfword. */
            unsigned len = ((w >> 1U) & 0x3FFU) + 1U;
            size_t bitpos = pos * 8U;
            unsigned i;

            if ((size_t)len > total - p) {
                return REPLAY_MALFORMED_STREAM;
            }
            for (i = 0U; i < len; ++i) {
                uint32_t pixel;

                if (!read_bits_lsb(payload, payload_size, bitpos, 15U,
                                   &pixel)) {
                    return REPLAY_TRUNCATED_INPUT;
                }
                decoded[p++] = (uint16_t)(pixel & 0x7FFFU);
                bitpos += 15U;
            }
            bitpos = (bitpos + 15U) & ~(size_t)15U; /* up to next 16-bit boundary */
            pos = bitpos / 8U;
        }
    }

    if (p != total) {
        return REPLAY_MALFORMED_STREAM; /* a frame must fill exactly one frame */
    }
    if (consumed != NULL) {
        *consumed = pos;
    }
    return REPLAY_OK;
}

static ReplayStatus put_word(ReplayBuffer *out, unsigned word)
{
    ReplayStatus status = replay_buffer_append_u8(out, (uint8_t)(word & 0xFFU));

    if (status == REPLAY_OK) {
        status = replay_buffer_append_u8(out, (uint8_t)((word >> 8U) & 0xFFU));
    }
    return status;
}

ReplayStatus codec_movinglines_encode_frame(const uint16_t *source,
                                            const uint16_t *previous,
                                            unsigned width, unsigned height,
                                            ReplayBuffer *out)
{
    size_t total;
    size_t p = 0U;
    size_t i;
    ReplayStatus status = REPLAY_OK;

    if (source == NULL || out == NULL || width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        return REPLAY_INVALID_ARGUMENT;
    }
    total = (size_t)width * height;
    for (i = 0U; i < total; ++i) {
        if (source[i] > CODEC_MOVINGLINES_PIXEL_MAX) {
            return REPLAY_INVALID_ARGUMENT; /* pixels must be 15-bit */
        }
    }
    replay_buffer_clear(out);

    while (status == REPLAY_OK && p < total) {
        if (previous != NULL && source[p] == previous[p]) {
            /* Same-position previous-frame copy, in spans of up to 1024. */
            size_t run = 1U;

            while (p + run < total && source[p + run] == previous[p + run]) {
                ++run;
            }
            while (status == REPLAY_OK && run != 0U) {
                unsigned chunk = run > 1024U ? 1024U : (unsigned)run;

                status = put_word(out, 1U + (0x1EU << 11U) +
                                           ((chunk - 1U) << 1U));
                p += chunk;
                run -= chunk;
            }
            continue;
        }

        {
            /* Run of identical pixels, stopping where a same-position copy
               would take over. */
            uint16_t pixel = source[p];
            size_t run = 1U;

            while (p + run < total && source[p + run] == pixel &&
                   !(previous != NULL && source[p + run] == previous[p + run])) {
                ++run;
            }
            if (run >= 3U) {
                unsigned chunk = run > 65U ? 65U : (unsigned)run;

                /* Never leave an un-repeatable tail of 1 or 2 pixels. */
                if (run - chunk == 1U || run - chunk == 2U) {
                    chunk = (unsigned)(run - 3U);
                }
                status = put_word(out, 1U + (0x1CCU << 7U) +
                                           ((chunk - 2U) << 1U));
                if (status == REPLAY_OK) {
                    status = put_word(out, pixel);
                }
                p += chunk;
                continue;
            }
        }

        /* Otherwise one literal pixel. */
        status = put_word(out, (unsigned)(source[p] & 0x7FFFU) << 1U);
        ++p;
    }

    if (status == REPLAY_OK) {
        status = put_word(out, ML_EOF_WORD);
    }
    if (status != REPLAY_OK) {
        replay_buffer_clear(out);
    }
    return status;
}
