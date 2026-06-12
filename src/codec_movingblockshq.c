#include "replay/codec_movingblockshq.h"

#include "replay/mb_frame_verify.h"

/*
 * Type 17 keeps YUV555 working pixels but replaces type 7's literal data
 * records with Huffman-coded luma prediction residuals. Its block syntax is
 * close to type 19, but the five-bit luma predictor is two-dimensional and
 * therefore remains a separate implementation.
 */
#define HUFF(bits_, count_) { UINT16_C(bits_), UINT8_C(count_) }

/* Source table entries are already in Replay's LSB-first stream order. */
static const MbHuffmanCode luma_codes[32] = {
    HUFF(0x002, 2), HUFF(0x007, 3), HUFF(0x004, 3), HUFF(0x008, 4),
    HUFF(0x01d, 5), HUFF(0x03b, 6), HUFF(0x035, 6), HUFF(0x05b, 7),
    HUFF(0x065, 7), HUFF(0x070, 7), HUFF(0x050, 7), HUFF(0x0ed, 8),
    HUFF(0x0a5, 8), HUFF(0x0c5, 8), HUFF(0x090, 8), HUFF(0x19b, 9),
    HUFF(0x16d, 9), HUFF(0x06d, 9), HUFF(0x09b, 9), HUFF(0x010, 8),
    HUFF(0x045, 8), HUFF(0x025, 8), HUFF(0x01b, 8), HUFF(0x030, 7),
    HUFF(0x005, 7), HUFF(0x02d, 7), HUFF(0x015, 6), HUFF(0x00d, 6),
    HUFF(0x000, 5), HUFF(0x00b, 5), HUFF(0x003, 4), HUFF(0x001, 3)
};

const MbHuffmanTable codec_movingblockshq_luma_huffman = {
    luma_codes, 32U, 9U
};

const MbCodec codec_movingblockshq = {
    REPLAY_CODEC_MOVINGBLOCKSHQ,
    "Moving Blocks HQ",
    MB_WORK_YUV555,
    5, 5, 5,
    4, 4,
    8, 8,
    &codec_movingblockshq_luma_huffman,
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
            error->detail = "truncated Moving Blocks HQ data header";
        }
        return status;
    }
    /*
     * Replay's LSB-first reader makes the first two stream bits bits 0..1 of
     * header. The remaining fields are therefore U in bits 2..6 and V in
     * bits 7..11. Keeping this as one read mirrors the generated decoder and
     * makes the on-wire layout explicit.
     */
    if ((header & UINT32_C(3)) != opcode) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader) - 12U;
            error->detail = "unexpected Moving Blocks HQ data opcode";
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
        reader, &codec_movingblockshq_luma_huffman, &symbol);

    if (status != REPLAY_OK) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader);
            error->detail =
                "invalid or truncated Moving Blocks HQ luma residual";
        }
        return status;
    }
    *residual = (uint8_t)symbol;
    return REPLAY_OK;
}

static uint8_t add5(unsigned prediction, uint8_t residual)
{
    /*
     * The stream has no signed-residual representation. A Huffman symbol is
     * the low five bits of the prediction error, so symbols 16..31 naturally
     * represent -16..-1 when the reconstructed result is reduced modulo 32.
     */
    return (uint8_t)((prediction + residual) & 31U);
}

ReplayStatus codec_movingblockshq_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    uint8_t u;
    uint8_t v;
    unsigned sum = 0U;
    unsigned row;
    ReplayStatus status;

    if (reader == NULL || predictor == NULL || pixels == NULL || stride < 4U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = read_header(reader, UINT32_C(1), &u, &v, error);
    for (row = 0U; status == REPLAY_OK && row < 4U; ++row) {
        unsigned column;

        for (column = 0U; status == REPLAY_OK && column < 4U; ++column) {
            uint8_t residual;
            unsigned prediction;
            MbPixel *pixel = &pixels[(size_t)row * stride + column];

            status = read_residual(reader, &residual, error);
            if (status != REPLAY_OK) {
                break;
            }
            /*
             * The first sample starts from the predictor carried between
             * data blocks. Thereafter the predictor follows already decoded
             * neighbours, so lossy residual substitution feeds back into the
             * rest of this block exactly as it does in the Acorn decoder.
             */
            if (row == 0U) {
                prediction = column == 0U
                                 ? predictor->luma
                                 : pixels[column - 1U].y;
            } else if (column == 0U) {
                prediction = pixels[(size_t)(row - 1U) * stride].y;
            } else {
                prediction =
                    ((unsigned)pixels[(size_t)row * stride + column - 1U].y +
                     (unsigned)pixels[(size_t)(row - 1U) * stride + column].y)
                    >> 1U;
            }
            pixel->y = add5(prediction, residual);
            pixel->u = u;
            pixel->v = v;
            sum += pixel->y;
        }
    }
    if (status == REPLAY_OK) {
        /* The generated decoder stores the truncated 16-pixel mean. */
        predictor->luma = (uint8_t)(sum >> 4U);
    }
    return status;
}

ReplayStatus codec_movingblockshq_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    uint8_t u;
    uint8_t v;
    uint8_t residual;
    unsigned sum = 0U;
    unsigned index;
    ReplayStatus status;

    if (reader == NULL || predictor == NULL || pixels == NULL || stride < 2U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = read_header(reader, UINT32_C(2), &u, &v, error);
    for (index = 0U; status == REPLAY_OK && index < 4U; ++index) {
        unsigned row = index >> 1U;
        unsigned column = index & 1U;
        unsigned prediction;
        MbPixel *pixel = &pixels[(size_t)row * stride + column];

        status = read_residual(reader, &residual, error);
        if (status != REPLAY_OK) {
            break;
        }
        /* The 2x2 predictor is a compact form of the same neighbour rule. */
        if (index == 0U) {
            prediction = predictor->luma;
        } else if (index == 1U) {
            prediction = pixels[0].y;
        } else if (index == 2U) {
            prediction = pixels[0].y;
        } else {
            prediction = ((unsigned)pixels[1].y +
                          (unsigned)pixels[stride].y) >> 1U;
        }
        pixel->y = add5(prediction, residual);
        pixel->u = u;
        pixel->v = v;
        sum += pixel->y;
    }
    if (status == REPLAY_OK) {
        /* The generated decoder stores the truncated four-pixel mean. */
        predictor->luma = (uint8_t)(sum >> 2U);
    }
    return status;
}

ReplayStatus codec_movingblockshq_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error)
{
    static const MbFrameVerifyCodec verifier = {
        codec_movingblockshq_decode_data4x4,
        codec_movingblockshq_decode_data2x2
    };

    return mb_frame_verify(&verifier, payload, payload_size, previous, decoded,
                           bits_consumed, error, NULL, NULL);
}
