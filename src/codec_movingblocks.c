#include "replay/codec_movingblocks.h"

#include <stddef.h>

#include "replay/mb_codec.h"
#include "replay/replay_bitstream.h"

/*
 * Compression type 7 is the original Moving Blocks format. This descriptor
 * records the source-derived working precision and nominal +/-4 search range.
 */
const MbCodec codec_movingblocks = {
    REPLAY_CODEC_MOVINGBLOCKS,
    "Moving Blocks",
    MB_WORK_YUV555,
    5, 5, 5,
    4, 4,
    4, 4,
    NULL,
    0, 0
};

static void set_error(MbVerifyError *error, ReplayBitReader *reader,
                      const char *detail)
{
    if (error != NULL) {
        error->bit_position = replay_bitreader_position(reader);
        error->detail = detail;
    }
}

/* Read one 5-bit field (the Y/U/V quantum of every type 7 literal block). */
static ReplayStatus read5(ReplayBitReader *reader, uint8_t *out)
{
    uint32_t value;
    ReplayStatus status = replay_bitreader_read(reader, 5U, &value);

    if (status == REPLAY_OK) {
        *out = (uint8_t)value;
    }
    return status;
}

ReplayStatus codec_movingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPixel *pixels, size_t stride,
    MbVerifyError *error)
{
    uint8_t y[16];
    uint8_t u;
    uint8_t v;
    size_t i;
    ReplayStatus status;

    if (reader == NULL || pixels == NULL || stride < 4U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /* Sixteen row-major Y values, then one shared U and V (90 bits total). */
    for (i = 0; i < 16U; ++i) {
        status = read5(reader, &y[i]);
        if (status != REPLAY_OK) {
            set_error(error, reader, "truncated 4x4 luma");
            return status;
        }
    }
    status = read5(reader, &u);
    if (status == REPLAY_OK) {
        status = read5(reader, &v);
    }
    if (status != REPLAY_OK) {
        set_error(error, reader, "truncated 4x4 chroma");
        return status;
    }
    for (i = 0; i < 16U; ++i) {
        MbPixel *out = &pixels[(i / 4U) * stride + (i % 4U)];

        out->y = y[i];
        out->u = u;
        out->v = v;
    }
    return REPLAY_OK;
}

ReplayStatus codec_movingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPixel *pixels, size_t stride,
    MbVerifyError *error)
{
    uint8_t y[4];
    uint8_t u;
    uint8_t v;
    size_t i;
    ReplayStatus status;

    if (reader == NULL || pixels == NULL || stride < 2U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /* Four row-major Y values, then one shared U and V (30 bits total). */
    for (i = 0; i < 4U; ++i) {
        status = read5(reader, &y[i]);
        if (status != REPLAY_OK) {
            set_error(error, reader, "truncated 2x2 luma");
            return status;
        }
    }
    status = read5(reader, &u);
    if (status == REPLAY_OK) {
        status = read5(reader, &v);
    }
    if (status != REPLAY_OK) {
        set_error(error, reader, "truncated 2x2 chroma");
        return status;
    }
    for (i = 0; i < 4U; ++i) {
        MbPixel *out = &pixels[(i / 2U) * stride + (i % 2U)];

        out->y = y[i];
        out->u = u;
        out->v = v;
    }
    return REPLAY_OK;
}

/* Verify the four 2x2 children of a split (top-level `01`) block. */
static ReplayStatus verify_split(ReplayBitReader *reader, MbFrame *decoded,
                                 unsigned x, unsigned y, MbVerifyError *error)
{
    static const unsigned offsets[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    unsigned block;

    for (block = 0; block < 4U; ++block) {
        unsigned block_x = x + offsets[block][0];
        unsigned block_y = y + offsets[block][1];
        uint32_t code;
        ReplayStatus status = replay_bitreader_read(reader, 1U, &code);

        if (status != REPLAY_OK) {
            set_error(error, reader, "truncated 2x2 opcode");
            return status;
        }
        if (code == 1U) {
            status = codec_movingblocks_decode_data2x2(
                reader,
                &decoded->pixels[(size_t)block_y * decoded->stride + block_x],
                decoded->stride, error);
            if (status != REPLAY_OK) {
                return status;
            }
        } else {
            /* `0` is a 2x2 move case -- motion is the next milestone. */
            set_error(error, reader, "type 7 motion not yet implemented");
            return REPLAY_MALFORMED_STREAM;
        }
    }
    return REPLAY_OK;
}

ReplayStatus codec_movingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error)
{
    ReplayBitReader reader;
    unsigned y;
    ReplayStatus status = REPLAY_OK;

    (void)previous; /* Motion (the only previous-frame use) is not yet decoded. */
    if (payload == NULL || decoded == NULL || decoded->pixels == NULL ||
        decoded->width == 0U || decoded->height == 0U ||
        (decoded->width & 3U) != 0U || (decoded->height & 3U) != 0U ||
        decoded->stride < decoded->width) {
        return REPLAY_INVALID_ARGUMENT;
    }
    replay_bitreader_init(&reader, payload, payload_size);
    /* Raster scan of 4x4 blocks; the top-level code is `1` data, `00` move,
       `01` split (read least-significant bit first). */
    for (y = 0; status == REPLAY_OK && y < decoded->height; y += 4U) {
        unsigned x;
        for (x = 0; status == REPLAY_OK && x < decoded->width; x += 4U) {
            uint32_t first;

            status = replay_bitreader_read(&reader, 1U, &first);
            if (status != REPLAY_OK) {
                set_error(error, &reader, "truncated 4x4 opcode");
                break;
            }
            if (first == 1U) {
                status = codec_movingblocks_decode_data4x4(
                    &reader,
                    &decoded->pixels[(size_t)y * decoded->stride + x],
                    decoded->stride, error);
            } else {
                uint32_t second;

                status = replay_bitreader_read(&reader, 1U, &second);
                if (status != REPLAY_OK) {
                    set_error(error, &reader, "truncated 4x4 opcode");
                    break;
                }
                if (second == 0U) {
                    /* `00` is a 4x4 move case -- the next milestone. */
                    set_error(error, &reader,
                              "type 7 motion not yet implemented");
                    status = REPLAY_MALFORMED_STREAM;
                } else {
                    status = verify_split(&reader, decoded, x, y, error);
                }
            }
        }
    }
    if (status == REPLAY_OK && bits_consumed != NULL) {
        *bits_consumed = replay_bitreader_position(&reader);
    }
    return status;
}
