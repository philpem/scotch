#include "replay/codec_supermovingblocks.h"

#define HUFF(bits_, count_) { UINT16_C(bits_), UINT8_C(count_) }

static const MbHuffmanCode luma_codes[64] = {
    HUFF(0x002, 2), HUFF(0x007, 3), HUFF(0x00d, 4), HUFF(0x019, 5),
    HUFF(0x01c, 5), HUFF(0x018, 5), HUFF(0x031, 6), HUFF(0x034, 6),
    HUFF(0x030, 6), HUFF(0x061, 7), HUFF(0x06c, 7), HUFF(0x050, 7),
    HUFF(0x0c1, 8), HUFF(0x0cc, 8), HUFF(0x0e8, 8), HUFF(0x1a1, 9),
    HUFF(0x18c, 9), HUFF(0x1d4, 9), HUFF(0x194, 9), HUFF(0x190, 9),
    HUFF(0x2a1, 10), HUFF(0x341, 10), HUFF(0x34c, 10), HUFF(0x294, 10),
    HUFF(0x314, 10), HUFF(0x541, 11), HUFF(0x141, 11), HUFF(0x68c, 11),
    HUFF(0x54c, 11), HUFF(0x14c, 11), HUFF(0x494, 11), HUFF(0x614, 11),
    HUFF(0x690, 11), HUFF(0x290, 11), HUFF(0x710, 11), HUFF(0x310, 11),
    HUFF(0x414, 11), HUFF(0x014, 11), HUFF(0x214, 11), HUFF(0x094, 11),
    HUFF(0x28c, 11), HUFF(0x090, 10), HUFF(0x110, 10), HUFF(0x114, 10),
    HUFF(0x08c, 10), HUFF(0x0a1, 10), HUFF(0x010, 9), HUFF(0x0d4, 9),
    HUFF(0x04c, 9), HUFF(0x041, 9), HUFF(0x068, 8), HUFF(0x054, 8),
    HUFF(0x00c, 8), HUFF(0x021, 8), HUFF(0x028, 7), HUFF(0x02c, 7),
    HUFF(0x001, 7), HUFF(0x008, 6), HUFF(0x011, 6), HUFF(0x000, 5),
    HUFF(0x004, 5), HUFF(0x009, 5), HUFF(0x005, 4), HUFF(0x003, 3)
};

const MbHuffmanTable codec_supermovingblocks_luma_huffman = {
    luma_codes,
    64,
    11
};

const MbCodec codec_supermovingblocks = {
    REPLAY_CODEC_SUPERMOVINGBLOCKS,
    "Super Moving Blocks",
    MB_WORK_6Y5UV,
    6, 5, 5,
    4, 4,
    8, 8,
    &codec_supermovingblocks_luma_huffman,
    0, 0
};

static ReplayStatus read_header(ReplayBitReader *reader, uint32_t opcode,
                                uint8_t *u, uint8_t *v,
                                MbVerifyError *error)
{
    uint32_t header;
    ReplayStatus status = replay_bitreader_read(reader, 12U, &header);

    if (status != REPLAY_OK) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader);
            error->detail = "truncated data-block header";
        }
        return status;
    }
    if ((header & UINT32_C(3)) != opcode) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader) - 12U;
            error->detail = "unexpected data-block opcode";
        }
        return REPLAY_MALFORMED_STREAM;
    }
    *u = (uint8_t)((header >> 2U) & UINT32_C(31));
    *v = (uint8_t)((header >> 7U) & UINT32_C(31));
    return REPLAY_OK;
}

static ReplayStatus read_residual(ReplayBitReader *reader, uint8_t *residual,
                                  MbVerifyError *error)
{
    unsigned symbol;
    ReplayStatus status = mb_huffman_read(
        reader, &codec_supermovingblocks_luma_huffman, &symbol);

    if (status != REPLAY_OK) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader);
            error->detail = "invalid or truncated luma residual";
        }
        return status;
    }
    *residual = (uint8_t)symbol;
    return REPLAY_OK;
}

static uint8_t add6(unsigned prediction, uint8_t residual)
{
    return (uint8_t)((prediction + residual) & 63U);
}

ReplayStatus codec_supermovingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    uint8_t u;
    uint8_t v;
    uint8_t y[4][4];
    unsigned sum = 0;
    size_t row;
    size_t column;
    ReplayStatus status;

    if (reader == NULL || predictor == NULL || pixels == NULL || stride < 4U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (error != NULL) {
        error->bit_position = replay_bitreader_position(reader);
        error->detail = NULL;
    }
    status = read_header(reader, UINT32_C(1), &u, &v, error);
    if (status != REPLAY_OK) {
        return status;
    }

    for (row = 0; row < 4U; ++row) {
        for (column = 0; column < 4U; ++column) {
            uint8_t residual;
            unsigned prediction;

            status = read_residual(reader, &residual, error);
            if (status != REPLAY_OK) {
                return status;
            }
            if (row == 0U) {
                prediction = column == 0U ? predictor->luma
                                          : y[row][column - 1U];
            } else if (column == 0U) {
                prediction = y[row - 1U][column];
            } else {
                prediction =
                    ((unsigned)y[row][column - 1U] +
                     (unsigned)y[row - 1U][column]) >> 1U;
            }
            y[row][column] = add6(prediction, residual);
            pixels[row * stride + column].y = y[row][column];
            pixels[row * stride + column].u = u;
            pixels[row * stride + column].v = v;
            sum += y[row][column];
        }
    }
    predictor->luma = (uint8_t)(sum >> 4U);
    return REPLAY_OK;
}

ReplayStatus codec_supermovingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    uint8_t u;
    uint8_t v;
    uint8_t residual;
    uint8_t y[4];
    ReplayStatus status;
    size_t i;

    if (reader == NULL || predictor == NULL || pixels == NULL || stride < 2U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (error != NULL) {
        error->bit_position = replay_bitreader_position(reader);
        error->detail = NULL;
    }
    status = read_header(reader, UINT32_C(2), &u, &v, error);
    if (status != REPLAY_OK) {
        return status;
    }

    status = read_residual(reader, &residual, error);
    if (status != REPLAY_OK) {
        return status;
    }
    y[0] = add6(predictor->luma, residual);
    status = read_residual(reader, &residual, error);
    if (status != REPLAY_OK) {
        return status;
    }
    y[1] = add6(y[0], residual);
    status = read_residual(reader, &residual, error);
    if (status != REPLAY_OK) {
        return status;
    }
    y[2] = add6(y[0], residual);
    status = read_residual(reader, &residual, error);
    if (status != REPLAY_OK) {
        return status;
    }
    y[3] = add6(((unsigned)y[1] + (unsigned)y[2]) >> 1U, residual);

    for (i = 0; i < 4U; ++i) {
        size_t offset = (i / 2U) * stride + (i % 2U);
        pixels[offset].y = y[i];
        pixels[offset].u = u;
        pixels[offset].v = v;
    }
    predictor->luma =
        (uint8_t)(((unsigned)y[0] + y[1] + y[2] + y[3]) >> 2U);
    return REPLAY_OK;
}

#undef HUFF
