#include "replay/codec_supermovingblocks.h"
#include "replay/mb_motion.h"

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
    1, 1
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

static ReplayStatus write_header(ReplayBitWriter *writer, uint32_t opcode,
                                 uint8_t u, uint8_t v)
{
    uint32_t header;

    if (writer == NULL || u > 31U || v > 31U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    header = opcode | ((uint32_t)u << 2U) | ((uint32_t)v << 7U);
    return replay_bitwriter_write(writer, header, 12U);
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

ReplayStatus codec_supermovingblocks_write_data4x4(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[16])
{
    size_t i;
    ReplayStatus status;

    if (writer == NULL || residuals == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = write_header(writer, UINT32_C(1), u, v);
    if (status != REPLAY_OK) {
        return status;
    }
    for (i = 0; i < 16U; ++i) {
        status = mb_huffman_write(writer,
                                  &codec_supermovingblocks_luma_huffman,
                                  residuals[i]);
        if (status != REPLAY_OK) {
            return status;
        }
    }
    return REPLAY_OK;
}

ReplayStatus codec_supermovingblocks_write_data2x2(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[4])
{
    size_t i;
    ReplayStatus status;

    if (writer == NULL || residuals == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = write_header(writer, UINT32_C(2), u, v);
    if (status != REPLAY_OK) {
        return status;
    }
    for (i = 0; i < 4U; ++i) {
        status = mb_huffman_write(writer,
                                  &codec_supermovingblocks_luma_huffman,
                                  residuals[i]);
        if (status != REPLAY_OK) {
            return status;
        }
    }
    return REPLAY_OK;
}

static int signed_chroma(uint8_t value)
{
    return value < 16U ? (int)value : (int)value - 32;
}

static uint8_t average_chroma4x4(const MbFrame *source, unsigned x,
                                 unsigned y, int use_v)
{
    int sum = 0;
    unsigned row;

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            const MbPixel *pixel =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            sum += signed_chroma(use_v != 0 ? pixel->v : pixel->u);
        }
    }
    if (sum < 0) {
        sum = -((-sum + 15) / 16);
    } else {
        sum /= 16;
    }
    return (uint8_t)(sum & 31);
}

static ReplayStatus make_data4x4(const MbFrame *source, unsigned x,
                                 unsigned y, MbPredictor *predictor,
                                 uint8_t residuals[16])
{
    unsigned sum = 0U;
    unsigned row;

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            const MbPixel *pixel =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            unsigned prediction;
            size_t index = row * 4U + column;

            if (pixel->y > 63U || pixel->u > 31U || pixel->v > 31U) {
                return REPLAY_INVALID_ARGUMENT;
            }
            if (row == 0U) {
                prediction = column == 0U
                                 ? predictor->luma
                                 : source->pixels[(size_t)y * source->stride +
                                                  x + column - 1U].y;
            } else if (column == 0U) {
                prediction =
                    source->pixels[(size_t)(y + row - 1U) * source->stride + x].y;
            } else {
                prediction =
                    ((unsigned)source->pixels[(size_t)(y + row) * source->stride +
                                             x + column - 1U].y +
                     (unsigned)source->pixels[(size_t)(y + row - 1U) *
                                                 source->stride +
                                             x + column].y) >>
                    1U;
            }
            residuals[index] = (uint8_t)((pixel->y - prediction) & 63U);
            sum += pixel->y;
        }
    }
    predictor->luma = (uint8_t)(sum >> 4U);
    return REPLAY_OK;
}

static void reconstruct_data4x4(const MbFrame *source, MbFrame *reconstructed,
                                unsigned x, unsigned y, uint8_t u, uint8_t v)
{
    unsigned row;

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            MbPixel *destination =
                &reconstructed->pixels[(size_t)(y + row) *
                                           reconstructed->stride +
                                       x + column];
            const MbPixel *input =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            destination->y = input->y;
            destination->u = u;
            destination->v = v;
        }
    }
}

static int block_matches_data_reconstruction(const MbFrame *source,
                                             const MbFrame *previous,
                                             unsigned x, unsigned y,
                                             uint8_t u, uint8_t v)
{
    unsigned row;

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            const MbPixel *input =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            const MbPixel *reference =
                &previous->pixels[(size_t)(y + row) * previous->stride +
                                  x + column];

            if (input->y != reference->y || u != reference->u ||
                v != reference->v) {
                return 0;
            }
        }
    }
    return 1;
}

static int temporal_block_matches(const MbFrame *source,
                                  const MbFrame *previous, unsigned x,
                                  unsigned y, int dx, int dy,
                                  uint8_t u, uint8_t v)
{
    int source_x = (int)x + dx;
    int source_y = (int)y + dy;
    unsigned row;

    if (source_x < 0 || source_y < 0 || source_x + 4 > (int)previous->width ||
        source_y + 4 > (int)previous->height) {
        return 0;
    }
    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            const MbPixel *target =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            const MbPixel *reference =
                &previous->pixels[((size_t)source_y + row) * previous->stride +
                                  (size_t)source_x + column];

            if (target->y != reference->y || u != reference->u ||
                v != reference->v) {
                return 0;
            }
        }
    }
    return 1;
}

static int find_temporal4x4(const MbFrame *source, const MbFrame *previous,
                            unsigned x, unsigned y, uint8_t u, uint8_t v,
                            MbMotionVector *motion)
{
    unsigned index;

    for (index = 0U; index < 288U; ++index) {
        if (mb_motion_format19_temporal_at(index, motion) != REPLAY_OK) {
            return 0;
        }
        if (temporal_block_matches(source, previous, x, y,
                                   motion->dx, motion->dy, u, v)) {
            return 1;
        }
    }
    return 0;
}

static void copy_temporal4x4(const MbFrame *previous, MbFrame *reconstructed,
                             unsigned x, unsigned y,
                             const MbMotionVector *motion)
{
    unsigned row;
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            reconstructed->pixels[(size_t)(y + row) * reconstructed->stride +
                                  x + column] =
                previous->pixels[(size_t)(source_y + row) * previous->stride +
                                 source_x + column];
        }
    }
}

static void copy_block4x4(const MbFrame *source, MbFrame *destination,
                          unsigned x, unsigned y)
{
    unsigned row;

    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] =
                source->pixels[(size_t)(y + row) * source->stride +
                               x + column];
        }
    }
}

static int valid_frame_pair(const MbFrame *source,
                            const MbFrame *reconstructed)
{
    return source != NULL && source->pixels != NULL &&
           reconstructed != NULL && reconstructed->pixels != NULL &&
           source->width != 0U && source->height != 0U &&
           (source->width & 3U) == 0U && (source->height & 3U) == 0U &&
           source->stride >= source->width &&
           reconstructed->width == source->width &&
           reconstructed->height == source->height &&
           reconstructed->stride >= reconstructed->width;
}

ReplayStatus codec_supermovingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecSuperMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecSuperMovingBlocksEncodeStats *stats)
{
    ReplayBitWriter writer;
    MbPredictor predictor = { 0 };
    CodecSuperMovingBlocksEncodeStats local_stats = { 0 };
    int allow_stationary = options != NULL && options->allow_stationary != 0;
    int allow_temporal = options != NULL && options->allow_temporal != 0;
    unsigned y;

    if (!valid_frame_pair(source, reconstructed) || output == NULL ||
        ((allow_stationary || allow_temporal) &&
         (previous == NULL || previous->pixels == NULL ||
          previous->width != source->width ||
          previous->height != source->height ||
          previous->stride < previous->width))) {
        return REPLAY_INVALID_ARGUMENT;
    }

    replay_buffer_clear(output);
    replay_bitwriter_init(&writer, output);
    for (y = 0; y < source->height; y += 4U) {
        unsigned x;
        for (x = 0; x < source->width; x += 4U) {
            uint8_t residuals[16];
            uint8_t u = average_chroma4x4(source, x, y, 0);
            uint8_t v = average_chroma4x4(source, x, y, 1);
            MbMotionVector motion;
            ReplayStatus status;

            if (allow_stationary && block_matches_data_reconstruction(
                                        source, previous, x, y, u, v)) {
                status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
                if (status == REPLAY_OK) {
                    copy_block4x4(previous, reconstructed, x, y);
                    ++local_stats.stationary4x4_blocks;
                }
            } else if (allow_temporal &&
                       find_temporal4x4(source, previous, x, y, u, v,
                                       &motion)) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(
                        &writer, MB_MOTION_BLOCK_4X4, &motion);
                }
                if (status == REPLAY_OK) {
                    copy_temporal4x4(previous, reconstructed, x, y, &motion);
                    ++local_stats.temporal4x4_blocks;
                }
            } else {
                status = make_data4x4(source, x, y, &predictor, residuals);
                if (status == REPLAY_OK) {
                    status = codec_supermovingblocks_write_data4x4(
                        &writer, u, v, residuals);
                }
                if (status == REPLAY_OK) {
                    reconstruct_data4x4(source, reconstructed, x, y, u, v);
                    ++local_stats.data4x4_blocks;
                }
            }
            if (status != REPLAY_OK) {
                replay_buffer_clear(output);
                return status;
            }
        }
    }
    local_stats.bits_written = replay_bitwriter_position(&writer);
    {
        ReplayStatus status = replay_bitwriter_flush_zero(&writer);
        if (status != REPLAY_OK) {
            replay_buffer_clear(output);
            return status;
        }
    }
    if (stats != NULL) {
        *stats = local_stats;
    }
    return REPLAY_OK;
}

ReplayStatus codec_supermovingblocks_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written)
{
    CodecSuperMovingBlocksEncodeOptions options = { 0, 0 };
    CodecSuperMovingBlocksEncodeStats stats;
    ReplayStatus status = codec_supermovingblocks_encode_frame(
        source, NULL, &options, output, reconstructed, &stats);

    if (status == REPLAY_OK && bits_written != NULL) {
        *bits_written = stats.bits_written;
    }
    return status;
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

static void set_verify_error(MbVerifyError *error, const ReplayBitReader *reader,
                             unsigned x, unsigned y, const char *detail)
{
    if (error != NULL) {
        error->bit_position = replay_bitreader_position(reader);
        error->block_x = x;
        error->block_y = y;
        error->detail = detail;
    }
}

static ReplayStatus copy_stationary(const MbFrame *previous, MbFrame *decoded,
                                    unsigned x, unsigned y, unsigned size,
                                    const ReplayBitReader *reader,
                                    MbVerifyError *error)
{
    unsigned row;

    if (previous == NULL || previous->pixels == NULL) {
        set_verify_error(error, reader, x, y,
                         "stationary block requires a previous frame");
        return REPLAY_MALFORMED_STREAM;
    }
    if (previous->width != decoded->width ||
        previous->height != decoded->height || previous->stride < decoded->width) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (row = 0; row < size; ++row) {
        unsigned column;
        for (column = 0; column < size; ++column) {
            decoded->pixels[(y + row) * decoded->stride + x + column] =
                previous->pixels[(y + row) * previous->stride + x + column];
        }
    }
    return REPLAY_OK;
}

static ReplayStatus copy_motion(ReplayBitReader *reader,
                                MbMotionBlockSize block_size,
                                const MbFrame *previous, MbFrame *decoded,
                                unsigned x, unsigned y, MbVerifyError *error)
{
    MbMotionVector motion;
    const MbFrame *source;
    int source_x;
    int source_y;
    unsigned row;
    ReplayStatus status =
        mb_motion_read_format19(reader, block_size, &motion);

    if (status != REPLAY_OK) {
        set_verify_error(error, reader, x, y,
                         "invalid or truncated motion code");
        return status;
    }
    source = motion.spatial ? decoded : previous;
    if (source == NULL || source->pixels == NULL) {
        set_verify_error(error, reader, x, y,
                         "temporal motion requires a previous frame");
        return REPLAY_MALFORMED_STREAM;
    }
    if (source->width != decoded->width || source->height != decoded->height ||
        source->stride < source->width) {
        return REPLAY_INVALID_ARGUMENT;
    }

    source_x = (int)x + motion.dx;
    source_y = (int)y + motion.dy;
    if (source_x < 0 || source_y < 0 ||
        source_x + (int)block_size > (int)source->width ||
        source_y + (int)block_size > (int)source->height) {
        set_verify_error(error, reader, x, y,
                         "motion reference lies outside the frame");
        return REPLAY_MALFORMED_STREAM;
    }

    for (row = 0; row < (unsigned)block_size; ++row) {
        unsigned column;
        for (column = 0; column < (unsigned)block_size; ++column) {
            decoded->pixels[(y + row) * decoded->stride + x + column] =
                source->pixels[((unsigned)source_y + row) * source->stride +
                               (unsigned)source_x + column];
        }
    }
    return REPLAY_OK;
}

static ReplayStatus verify_2x2(ReplayBitReader *reader,
                               MbPredictor *predictor,
                               const MbFrame *previous, MbFrame *decoded,
                               unsigned x, unsigned y, MbVerifyError *error)
{
    ReplayBitReader start = *reader;
    uint32_t first;
    ReplayStatus status = replay_bitreader_read(reader, 1U, &first);

    if (status != REPLAY_OK) {
        set_verify_error(error, reader, x, y, "truncated 2x2 opcode");
        return status;
    }
    if (first != 0U) {
        return copy_motion(reader, MB_MOTION_BLOCK_2X2, previous, decoded,
                           x, y, error);
    }

    {
        uint32_t second;
        status = replay_bitreader_read(reader, 1U, &second);
        if (status != REPLAY_OK) {
            set_verify_error(error, reader, x, y, "truncated 2x2 opcode");
            return status;
        }
        if (second == 0U) {
            return copy_stationary(previous, decoded, x, y, 2U, reader, error);
        }
    }

    *reader = start;
    status = codec_supermovingblocks_decode_data2x2(
        reader, predictor, decoded->pixels + y * decoded->stride + x,
        decoded->stride, error);
    if (status != REPLAY_OK && error != NULL) {
        error->block_x = x;
        error->block_y = y;
    }
    return status;
}

ReplayStatus codec_supermovingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error)
{
    ReplayBitReader reader;
    MbPredictor predictor = { 0 };
    unsigned y;

    if ((payload == NULL && payload_size != 0U) || decoded == NULL ||
        decoded->pixels == NULL || decoded->width == 0U ||
        decoded->height == 0U || decoded->stride < decoded->width ||
        (decoded->width & 3U) != 0U || (decoded->height & 3U) != 0U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (error != NULL) {
        error->bit_position = 0U;
        error->block_x = 0U;
        error->block_y = 0U;
        error->detail = NULL;
    }
    replay_bitreader_init(&reader, payload, payload_size);

    for (y = 0; y < decoded->height; y += 4U) {
        unsigned x;
        for (x = 0; x < decoded->width; x += 4U) {
            ReplayBitReader start = reader;
            uint32_t opcode;
            ReplayStatus status = replay_bitreader_read(&reader, 2U, &opcode);

            if (status != REPLAY_OK) {
                set_verify_error(error, &reader, x, y, "truncated 4x4 opcode");
                return status;
            }
            switch (opcode) {
            case 0U:
                status = copy_stationary(previous, decoded, x, y, 4U,
                                         &reader, error);
                break;
            case 1U:
                reader = start;
                status = codec_supermovingblocks_decode_data4x4(
                    &reader, &predictor,
                    decoded->pixels + y * decoded->stride + x,
                    decoded->stride, error);
                if (status != REPLAY_OK && error != NULL) {
                    error->block_x = x;
                    error->block_y = y;
                }
                break;
            case 2U:
                status = copy_motion(&reader, MB_MOTION_BLOCK_4X4,
                                     previous, decoded, x, y, error);
                break;
            case 3U:
                status = verify_2x2(&reader, &predictor, previous, decoded,
                                    x, y, error);
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x + 2U, y, error);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x, y + 2U, error);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x + 2U, y + 2U, error);
                }
                break;
            default:
                status = REPLAY_INTERNAL_ERROR;
                break;
            }
            if (status != REPLAY_OK) {
                return status;
            }
        }
    }
    if (bits_consumed != NULL) {
        *bits_consumed = replay_bitreader_position(&reader);
    }
    {
        size_t used_bits = replay_bitreader_position(&reader);
        size_t used_bytes = (used_bits + 7U) / 8U;

        if (used_bytes != payload_size) {
            set_verify_error(error, &reader, decoded->width - 4U,
                             decoded->height - 4U,
                             "payload has trailing bytes");
            return REPLAY_MALFORMED_STREAM;
        }
        if ((used_bits & 7U) != 0U &&
            (payload[used_bits / 8U] >> (used_bits & 7U)) != 0U) {
            set_verify_error(error, &reader, decoded->width - 4U,
                             decoded->height - 4U,
                             "payload has non-zero padding bits");
            return REPLAY_MALFORMED_STREAM;
        }
    }
    return REPLAY_OK;
}

#undef HUFF
