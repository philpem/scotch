#include "replay/codec_supermovingblocks.h"
#include "replay/mb_encode.h"
#include "replay/mb_frame_verify.h"
#include "replay/mb_motion.h"
#include "replay/mb_quality.h"

#include <stdlib.h>
#include <string.h>

/*
 * Format 19 scans 4x4 blocks left-to-right, top-to-bottom. Each block starts
 * with a two-bit opcode:
 *
 *   00 stationary 4x4          01 data 4x4
 *   10 motion/spatial 4x4      11 split into four 2x2 blocks
 *
 * Bits within fields are emitted least-significant first. Data blocks carry
 * one signed five-bit U/V pair for the whole block and Huffman-coded six-bit
 * luma residuals. Copy blocks do not alter the luma predictor.
 *
 * A key frame is independently decodable: it may contain data and spatial
 * blocks, because spatial references stay within the current frame. An inter
 * frame may additionally contain stationary and temporal blocks referencing
 * the previous reconstructed frame. In MPEG terminology these dependencies
 * are broadly I-frame-like and P-frame-like respectively; there is no future
 * reference comparable to an MPEG B-frame.
 *
 * Copy acceptance uses the original 29-row quality table. Level zero requires
 * exact decoder-visible pixels; higher levels permit the bounded luma/chroma
 * error implemented in mb_quality.c. Whole-frame target-size retries are kept
 * outside this codec core in mb_rate_control.c, so one call is deterministic.
 *
 * Copy-family selection is explicit. The original portable policy tries
 * stationary, temporal, then spatial. The experimental lowest-error policy
 * compares every accepted copy by reconstruction error, emitted bits, then a
 * stable family/table order. Neither policy changes the bitstream syntax.
 */
#define HUFF(bits_, count_) { UINT16_C(bits_), UINT8_C(count_) }

/* Indexed by the residual's six-bit modulo representation, 0..63. */
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
    /* Header layout is opcode:2, U:5, V:5 in stream-bit order. */
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
    /* Residuals are modulo 64, so symbols 32..63 represent negative deltas. */
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
            /*
             * Predictor topology from the Acorn compressor:
             * - first pixel: previous data block's average luma;
             * - remaining top row: pixel immediately to the left;
             * - first pixel of later rows: pixel immediately above;
             * - interior: floor((left + above) / 2).
             */
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
    /* Only a data block advances this cross-block predictor. */
    predictor->luma = (uint8_t)(sum >> 4U);
    return REPLAY_OK;
}

static ReplayStatus make_data2x2(const MbFrame *source, unsigned x,
                                 unsigned y, MbPredictor *predictor,
                                 uint8_t residuals[4])
{
    const MbPixel *top_left =
        &source->pixels[(size_t)y * source->stride + x];
    const MbPixel *top_right = top_left + 1U;
    const MbPixel *bottom_left = top_left + source->stride;
    const MbPixel *bottom_right = bottom_left + 1U;
    unsigned prediction;

    if (top_left->y > 63U || top_right->y > 63U ||
        bottom_left->y > 63U || bottom_right->y > 63U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /* 2x2 uses the same topology reduced to four pixels. */
    residuals[0] = (uint8_t)((top_left->y - predictor->luma) & 63U);
    residuals[1] = (uint8_t)((top_right->y - top_left->y) & 63U);
    residuals[2] = (uint8_t)((bottom_left->y - top_left->y) & 63U);
    prediction = ((unsigned)top_right->y + bottom_left->y) >> 1U;
    residuals[3] = (uint8_t)((bottom_right->y - prediction) & 63U);
    predictor->luma =
        (uint8_t)(((unsigned)top_left->y + top_right->y + bottom_left->y +
                   bottom_right->y) >> 2U);
    return REPLAY_OK;
}

/*
 * Type-19 instance of the shared search hook table. The wrappers adapt the
 * 6Y5UV motion tables and quality model to the codec-neutral signatures in
 * mb_encode.h; the search itself never names a format-19 symbol.
 */
static int codec19_temporal_vector(unsigned index, MbMotionVector *out)
{
    return mb_motion_format19_temporal_at(index, out) == REPLAY_OK;
}

static int codec19_spatial_vector(MbMotionBlockSize block_size, unsigned index,
                                  MbMotionVector *out)
{
    return mb_motion_format19_spatial_at(block_size, index, out) == REPLAY_OK;
}

static int codec19_block_match(const MbFrame *source, unsigned x, unsigned y,
                               const MbFrame *reference, unsigned ref_x,
                               unsigned ref_y, unsigned size, uint8_t u,
                               uint8_t v, const void *quality, unsigned *error)
{
    return mb_quality_match_format19(source, x, y, reference, ref_x, ref_y,
                                     size, u, v,
                                     (const MbQualityThresholds *)quality,
                                     error);
}

static int codec19_profile_match(const MbFrame *source, unsigned x, unsigned y,
                                 const MbFrame *reference, unsigned ref_x,
                                 unsigned ref_y, unsigned size, uint8_t u,
                                 uint8_t v, unsigned *total_error,
                                 unsigned *first_level)
{
    MbQualityProfile profile;

    if (!mb_quality_profile_format19(source, x, y, reference, ref_x, ref_y,
                                     size, u, v, &profile)) {
        return 0;
    }
    *total_error = profile.total_error;
    *first_level = mb_quality_first_accepted_level(&profile, size);
    return 1;
}

static const MbEncodeCodec codec19_encode = {
    MB_QUALITY_LEVEL_COUNT,
    288U,
    8U,
    codec19_temporal_vector,
    codec19_spatial_vector,
    codec19_block_match,
    codec19_profile_match,
    mb_encode_motion_bits
};

/*
 * Data-block coding for the shared frame encoder: write one size x size data
 * block and reconstruct it. Luma is lossless at six bits, so the predictor
 * residuals reproduce the source luma exactly and the reconstruction is the
 * source luma plus the block-average chroma.
 */
static ReplayStatus codec19_encode_data(ReplayBitWriter *writer,
                                        const MbFrame *source, unsigned x,
                                        unsigned y, unsigned size,
                                        MbPixel *recon, size_t recon_stride,
                                        MbPredictor *predictor)
{
    uint8_t u = mb_encode_average_chroma(source, x, y, size, 0);
    uint8_t v = mb_encode_average_chroma(source, x, y, size, 1);
    ReplayStatus status;
    unsigned row;

    if (size == 4U) {
        uint8_t residuals[16];

        status = make_data4x4(source, x, y, predictor, residuals);
        if (status == REPLAY_OK) {
            status = codec_supermovingblocks_write_data4x4(writer, u, v,
                                                           residuals);
        }
    } else {
        uint8_t residuals[4];

        status = make_data2x2(source, x, y, predictor, residuals);
        if (status == REPLAY_OK) {
            status = codec_supermovingblocks_write_data2x2(writer, u, v,
                                                           residuals);
        }
    }
    if (status != REPLAY_OK) {
        return status;
    }
    for (row = 0U; row < size; ++row) {
        unsigned column;
        for (column = 0U; column < size; ++column) {
            MbPixel *out = &recon[(size_t)row * recon_stride + column];

            out->y = source->pixels[(size_t)(y + row) * source->stride +
                                    x + column].y;
            out->u = u;
            out->v = v;
        }
    }
    return REPLAY_OK;
}

static const MbEncodeDataCodec codec19_data = { codec19_encode_data };

/* The shared motion-search cache (mb_encode.c) is held behind the opaque
 * public handle; these entry points just own its lifetime. */
static MbEncodeWorkspace *workspace_internal(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    return workspace != NULL ? (MbEncodeWorkspace *)workspace->internal : NULL;
}

ReplayStatus codec_supermovingblocks_workspace_init(
    CodecSuperMovingBlocksWorkspace *workspace, unsigned width,
    unsigned height)
{
    MbEncodeWorkspace *internal;
    ReplayStatus status;

    if (workspace == NULL || workspace->internal != NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    internal = calloc(1U, sizeof(*internal));
    if (internal == NULL) {
        return REPLAY_OUT_OF_MEMORY;
    }
    status = mb_encode_workspace_init(internal, width, height);
    if (status != REPLAY_OK) {
        free(internal);
        return status;
    }
    workspace->internal = internal;
    return REPLAY_OK;
}

void codec_supermovingblocks_workspace_reset(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    mb_encode_workspace_reset(workspace_internal(workspace));
}

void codec_supermovingblocks_workspace_destroy(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    MbEncodeWorkspace *internal = workspace_internal(workspace);

    if (internal != NULL) {
        mb_encode_workspace_destroy(internal);
        free(internal);
        workspace->internal = NULL;
    }
}

static int valid_source_samples(const MbFrame *source)
{
    unsigned y;

    for (y = 0U; y < source->height; ++y) {
        unsigned x;
        for (x = 0U; x < source->width; ++x) {
            const MbPixel *pixel =
                &source->pixels[(size_t)y * source->stride + x];
            if (pixel->y > 63U || pixel->u > 31U || pixel->v > 31U) {
                return 0;
            }
        }
    }
    return 1;
}

ReplayStatus codec_supermovingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecSuperMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecSuperMovingBlocksEncodeStats *stats)
{
    MbEncodeOptions encode_options;
    MbEncodeStats encode_stats;
    CodecSuperMovingBlocksPolicy policy =
        options != NULL ? options->policy
                        : CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED;
    ReplayStatus status;

    /*
     * The shared frame encoder (mb_encode_frame) does the block grammar, motion
     * search, quality model and size decision; this adapter adds only the
     * format-19 specifics: the 6Y5UV source-range check, the policy mapping, and
     * the data-block coder. Stats are copied field-for-field into the public
     * struct.
     */
    if (output != NULL) {
        replay_buffer_clear(output);
    }
    if (source == NULL || source->pixels == NULL ||
        !valid_source_samples(source) ||
        (policy != CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED &&
         policy != CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR)) {
        return REPLAY_INVALID_ARGUMENT;
    }

    encode_options.allow_stationary =
        options != NULL && options->allow_stationary != 0;
    encode_options.allow_temporal =
        options != NULL && options->allow_temporal != 0;
    encode_options.allow_spatial =
        options != NULL && options->allow_spatial != 0;
    encode_options.allow_split = options != NULL && options->allow_split != 0;
    encode_options.loss_level = options != NULL ? options->loss_level : 0U;
    encode_options.policy =
        policy == CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR
            ? MB_ENCODE_POLICY_LOWEST_ERROR
            : MB_ENCODE_POLICY_ORDERED;
    encode_options.workspace =
        options != NULL ? workspace_internal(options->workspace) : NULL;

    status = mb_encode_frame(&codec19_encode, &codec19_data, source, previous,
                             &encode_options, output, reconstructed,
                             &encode_stats);
    if (status == REPLAY_OK && stats != NULL) {
        stats->data4x4_blocks = encode_stats.data4x4_blocks;
        stats->stationary4x4_blocks = encode_stats.stationary4x4_blocks;
        stats->temporal4x4_blocks = encode_stats.temporal4x4_blocks;
        stats->spatial4x4_blocks = encode_stats.spatial4x4_blocks;
        stats->split4x4_blocks = encode_stats.split4x4_blocks;
        stats->data2x2_blocks = encode_stats.data2x2_blocks;
        stats->stationary2x2_blocks = encode_stats.stationary2x2_blocks;
        stats->temporal2x2_blocks = encode_stats.temporal2x2_blocks;
        stats->spatial2x2_blocks = encode_stats.spatial2x2_blocks;
        stats->stationary4x4_evaluations =
            encode_stats.stationary4x4_evaluations;
        stats->temporal4x4_evaluations = encode_stats.temporal4x4_evaluations;
        stats->spatial4x4_evaluations = encode_stats.spatial4x4_evaluations;
        stats->stationary2x2_evaluations =
            encode_stats.stationary2x2_evaluations;
        stats->temporal2x2_evaluations = encode_stats.temporal2x2_evaluations;
        stats->spatial2x2_evaluations = encode_stats.spatial2x2_evaluations;
        stats->bits_written = encode_stats.bits_written;
    }
    return status;
}

ReplayStatus codec_supermovingblocks_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written)
{
    CodecSuperMovingBlocksEncodeOptions options = {
        0, 0, 0, 0, 0U, CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED, NULL
    };
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

    /* Decode in the same raster order used to form predictor residuals. */
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

typedef struct {
    CodecSuperMovingBlocksDecodeTrace trace;
    void *opaque;
} SuperMovingBlocksTraceAdapter;

static void adapt_decode_event(const MbDecodeEvent *source, void *opaque)
{
    SuperMovingBlocksTraceAdapter *adapter =
        (SuperMovingBlocksTraceAdapter *)opaque;
    CodecSuperMovingBlocksDecodeEvent event;

    event.x = source->x;
    event.y = source->y;
    event.size = source->size;
    /* Keep the public type-19 API independent of the shared enum's values. */
    switch (source->mode) {
    case MB_DECODE_MODE_DATA:
        event.mode = CODEC_SUPERMOVINGBLOCKS_MODE_DATA;
        break;
    case MB_DECODE_MODE_STATIONARY:
        event.mode = CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY;
        break;
    case MB_DECODE_MODE_TEMPORAL:
        event.mode = CODEC_SUPERMOVINGBLOCKS_MODE_TEMPORAL;
        break;
    case MB_DECODE_MODE_SPATIAL:
        event.mode = CODEC_SUPERMOVINGBLOCKS_MODE_SPATIAL;
        break;
    case MB_DECODE_MODE_SPLIT:
        event.mode = CODEC_SUPERMOVINGBLOCKS_MODE_SPLIT;
        break;
    default:
        return;
    }
    event.bit_start = source->bit_start;
    event.bit_end = source->bit_end;
    event.motion_dx = source->motion_dx;
    event.motion_dy = source->motion_dy;
    adapter->trace(&event, adapter->opaque);
}

ReplayStatus codec_supermovingblocks_verify_frame_traced(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    CodecSuperMovingBlocksDecodeTrace trace, void *trace_opaque)
{
    static const MbFrameVerifyCodec verifier = {
        codec_supermovingblocks_decode_data4x4,
        codec_supermovingblocks_decode_data2x2
    };
    SuperMovingBlocksTraceAdapter adapter = { trace, trace_opaque };

    return mb_frame_verify(
        &verifier, payload, payload_size, previous, decoded,
        bits_consumed, error,
        trace != NULL ? adapt_decode_event : NULL, &adapter);
}

ReplayStatus codec_supermovingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error)
{
    return codec_supermovingblocks_verify_frame_traced(
        payload, payload_size, previous, decoded, bits_consumed, error,
        NULL, NULL);
}

#undef HUFF
