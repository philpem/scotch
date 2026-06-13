#include "replay/codec_movingblocksbeta.h"

#include <stddef.h>
#include <stdint.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_codec.h"
#include "replay/mb_encode.h"
#include "replay/mb_frame_verify.h"
#include "replay/mb_huffman.h"
#include "replay/mb_motion.h"
#include "replay/mb_quality.h"
#include "replay/replay_bitstream.h"
#include "replay/replay_buffer.h"

/*
 * Type 20 is the 6Y6UV beta branch. It reuses type 19's grammar, motion tables
 * and luma Huffman table; only the chroma differs. Two type 20 decoders shipped:
 * the v0.04 "old" module (20 Sep 1996) stores chroma directly as six bits per
 * component, and the v0.05 "new" module (19 Nov 1996) delta-codes it. Both are
 * implemented, selected by CodecMovingBlocksBetaVariant; see
 * docs/type20-shipped-vs-source.md.
 */
const MbCodec codec_movingblocksbeta = {
    REPLAY_CODEC_MOVINGBLOCKSBETA,
    "Moving Blocks Beta",
    MB_WORK_6Y6UV,
    6, 6, 6,
    4, 4,
    8, 8,
    &codec_supermovingblocks_luma_huffman,
    0, 0
};

/* ---------------------------------------------------------------------- *
 * Decode.
 * ---------------------------------------------------------------------- */

/* "new" (v0.05) chroma delta table (Decomp20 `deltaexpand`), indexed 0..15. */
static const int8_t delta_expand[16] = {
    -32, -26, -20, -14, -8, -4, -2, -1, 0, 1, 2, 4, 8, 14, 20, 26
};

/*
 * "old" (v0.04) data header: opcode (2 bits), then U and V as six-bit fields
 * (14 bits total), matching the 12456-byte module's `add r0,#14` + 12-bit chroma
 * extract. The chroma is the output word's U<<6|V<<12, i.e. U in bits 2..7 and
 * V in 8..13, stored directly.
 */
static ReplayStatus read_header_old(ReplayBitReader *reader, uint32_t opcode,
                                    MbPredictor *predictor, uint8_t *u,
                                    uint8_t *v, MbVerifyError *error)
{
    uint32_t header;
    ReplayStatus status = replay_bitreader_read(reader, 14U, &header);

    (void)predictor;
    if (status != REPLAY_OK) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader);
            error->detail = "truncated Moving Blocks Beta data header";
        }
        return status;
    }
    if ((header & UINT32_C(3)) != opcode) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader) - 14U;
            error->detail = "unexpected Moving Blocks Beta data opcode";
        }
        return REPLAY_MALFORMED_STREAM;
    }
    *u = (uint8_t)((header >> 2U) & 63U);
    *v = (uint8_t)((header >> 8U) & 63U);
    return REPLAY_OK;
}

/*
 * "new" (v0.05) data header: opcode (2 bits) + an 8-bit uv byte (10 bits total,
 * the 12528-byte module's `add r0,#0xa` + `unpackuv`). The uv byte holds a
 * 4-bit delta code per component (u low nibble, v high), added to the chroma
 * predictor carried across data blocks and masked to six bits.
 */
static ReplayStatus read_header_new(ReplayBitReader *reader, uint32_t opcode,
                                    MbPredictor *predictor, uint8_t *u,
                                    uint8_t *v, MbVerifyError *error)
{
    uint32_t header;
    unsigned uv;
    ReplayStatus status = replay_bitreader_read(reader, 10U, &header);

    if (status != REPLAY_OK) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader);
            error->detail = "truncated Moving Blocks Beta data header";
        }
        return status;
    }
    if ((header & UINT32_C(3)) != opcode) {
        if (error != NULL) {
            error->bit_position = replay_bitreader_position(reader) - 10U;
            error->detail = "unexpected Moving Blocks Beta data opcode";
        }
        return REPLAY_MALFORMED_STREAM;
    }
    uv = (header >> 2U) & 0xFFU;
    *u = (uint8_t)(((unsigned)predictor->chroma_u +
                    (unsigned)delta_expand[uv & 15U]) & 63U);
    *v = (uint8_t)(((unsigned)predictor->chroma_v +
                    (unsigned)delta_expand[(uv >> 4U) & 15U]) & 63U);
    predictor->chroma_u = *u;
    predictor->chroma_v = *v;
    return REPLAY_OK;
}

typedef ReplayStatus (*ReadHeaderFn)(ReplayBitReader *, uint32_t, MbPredictor *,
                                     uint8_t *, uint8_t *, MbVerifyError *);

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

static ReplayStatus decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error, ReadHeaderFn read_hdr)
{
    uint8_t u;
    uint8_t v;
    uint8_t y[4][4];
    unsigned sum = 0U;
    size_t row;
    size_t column;
    ReplayStatus status;

    if (reader == NULL || predictor == NULL || pixels == NULL || stride < 4U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = read_hdr(reader, UINT32_C(1), predictor, &u, &v, error);
    if (status != REPLAY_OK) {
        return status;
    }
    /* Luma is decoded exactly as in type 19 (same predictor topology). */
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
                prediction = ((unsigned)y[row][column - 1U] +
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

static ReplayStatus decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error, ReadHeaderFn read_hdr)
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
    status = read_hdr(reader, UINT32_C(2), predictor, &u, &v, error);
    if (status != REPLAY_OK) {
        return status;
    }
    status = read_residual(reader, &residual, error);
    if (status == REPLAY_OK) {
        y[0] = add6(predictor->luma, residual);
        status = read_residual(reader, &residual, error);
    }
    if (status == REPLAY_OK) {
        y[1] = add6(y[0], residual);
        status = read_residual(reader, &residual, error);
    }
    if (status == REPLAY_OK) {
        y[2] = add6(y[0], residual);
        status = read_residual(reader, &residual, error);
    }
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
    predictor->luma = (uint8_t)(((unsigned)y[0] + y[1] + y[2] + y[3]) >> 2U);
    return REPLAY_OK;
}

/* The public primitives decode the "old" (direct chroma) format. */
ReplayStatus codec_movingblocksbeta_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    return decode_data4x4(reader, predictor, pixels, stride, error,
                          read_header_old);
}

ReplayStatus codec_movingblocksbeta_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    return decode_data2x2(reader, predictor, pixels, stride, error,
                          read_header_old);
}

static ReplayStatus decode_data4x4_new(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    return decode_data4x4(reader, predictor, pixels, stride, error,
                          read_header_new);
}

static ReplayStatus decode_data2x2_new(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error)
{
    return decode_data2x2(reader, predictor, pixels, stride, error,
                          read_header_new);
}

ReplayStatus codec_movingblocksbeta_verify_frame_variant(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    CodecMovingBlocksBetaVariant variant)
{
    static const MbFrameVerifyCodec verifier_old = {
        codec_movingblocksbeta_decode_data4x4,
        codec_movingblocksbeta_decode_data2x2
    };
    static const MbFrameVerifyCodec verifier_new = {
        decode_data4x4_new, decode_data2x2_new
    };

    return mb_frame_verify(
        variant == CODEC_MOVINGBLOCKSBETA_NEW ? &verifier_new : &verifier_old,
        payload, payload_size, previous, decoded, bits_consumed, error, NULL,
        NULL);
}

ReplayStatus codec_movingblocksbeta_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error)
{
    return codec_movingblocksbeta_verify_frame_variant(
        payload, payload_size, previous, decoded, bits_consumed, error,
        CODEC_MOVINGBLOCKSBETA_OLD);
}

/* ---------------------------------------------------------------------- *
 * Encode.
 * ---------------------------------------------------------------------- */

static int codec20_temporal_vector(unsigned index, MbMotionVector *out)
{
    return mb_motion_format19_temporal_at(index, out) == REPLAY_OK;
}

static int codec20_spatial_vector(MbMotionBlockSize block_size, unsigned index,
                                  MbMotionVector *out)
{
    return mb_motion_format19_spatial_at(block_size, index, out) == REPLAY_OK;
}

static int codec20_block_match(const MbFrame *source, unsigned x, unsigned y,
                               const MbFrame *reference, unsigned ref_x,
                               unsigned ref_y, unsigned size, uint8_t u,
                               uint8_t v, const void *quality, unsigned *error)
{
    return mb_quality_match_6y6uv(source, x, y, reference, ref_x, ref_y, size,
                                  u, v, (const MbQualityThresholds *)quality,
                                  error);
}

static int codec20_profile_match(const MbFrame *source, unsigned x, unsigned y,
                                 const MbFrame *reference, unsigned ref_x,
                                 unsigned ref_y, unsigned size, uint8_t u,
                                 uint8_t v, unsigned *total_error,
                                 unsigned *first_level)
{
    MbQualityProfile profile;

    if (!mb_quality_profile_6y6uv(source, x, y, reference, ref_x, ref_y, size,
                                  u, v, &profile)) {
        return 0;
    }
    *total_error = profile.total_error;
    *first_level = mb_quality_first_accepted_level(&profile, size);
    return 1;
}

static const MbEncodeCodec codec20_encode = {
    MB_QUALITY_LEVEL_COUNT,
    288U,
    8U,
    codec20_temporal_vector,
    codec20_spatial_vector,
    codec20_block_match,
    codec20_profile_match,
    mb_encode_motion_bits,
    32
};

/* Encode one data block's luma residuals (same topology as type 19), writing
 * them to `residuals` and advancing the luma predictor. */
static void make_luma(const MbFrame *source, unsigned x, unsigned y,
                      unsigned size, MbPredictor *predictor, uint8_t *residuals)
{
    unsigned sum = 0U;
    unsigned row;

    for (row = 0U; row < size; ++row) {
        unsigned column;
        for (column = 0U; column < size; ++column) {
            unsigned target = source->pixels[(size_t)(y + row) *
                                                  source->stride + x + column].y;
            unsigned prediction;

            if (row == 0U && column == 0U) {
                prediction = predictor->luma;
            } else if (size == 2U && row == 1U && column == 1U) {
                prediction =
                    ((unsigned)source->pixels[(size_t)y * source->stride +
                                              x + 1U].y +
                     source->pixels[(size_t)(y + 1U) * source->stride + x].y)
                    >> 1U;
            } else if (row == 0U) {
                prediction = source->pixels[(size_t)y * source->stride +
                                            x + column - 1U].y;
            } else if (column == 0U) {
                prediction = source->pixels[(size_t)(y + row - 1U) *
                                                source->stride + x].y;
            } else {
                prediction =
                    ((unsigned)source->pixels[(size_t)(y + row) *
                                                  source->stride +
                                              x + column - 1U].y +
                     source->pixels[(size_t)(y + row - 1U) * source->stride +
                                    x + column].y) >> 1U;
            }
            residuals[row * size + column] =
                (uint8_t)((target - prediction) & 63U);
            sum += target;
        }
    }
    predictor->luma = (uint8_t)(sum >> (size == 4U ? 4U : 2U));
}

static int sign6(uint8_t value)
{
    return value < 32U ? (int)value : (int)value - 64;
}

/* "new" variant: pick the 4-bit delta code whose reconstruction is closest to
 * the target six-bit chroma, advance the predictor, and return the code. */
static unsigned choose_delta(uint8_t target, uint8_t *predictor)
{
    int target_signed = sign6(target);
    unsigned best_code = 0U;
    int best_error = 1 << 20;
    uint8_t best_value = *predictor;
    unsigned code;

    for (code = 0U; code < 16U; ++code) {
        uint8_t candidate =
            (uint8_t)(((unsigned)*predictor + (unsigned)delta_expand[code]) &
                      63U);
        int error = sign6(candidate) - target_signed;

        if (error < 0) {
            error = -error;
        }
        if (error < best_error) {
            best_error = error;
            best_code = code;
            best_value = candidate;
        }
    }
    *predictor = best_value;
    return best_code;
}

/* Write the luma residuals (after the header) and reconstruct the block: luma
 * lossless, chroma the supplied u, v. Shared by both variants. */
static ReplayStatus write_luma_recon(ReplayBitWriter *writer,
                                     const MbFrame *source, unsigned x,
                                     unsigned y, unsigned size, MbPixel *recon,
                                     size_t recon_stride, MbPredictor *predictor,
                                     uint8_t u, uint8_t v)
{
    uint8_t residuals[16];
    ReplayStatus status = REPLAY_OK;
    unsigned i;
    unsigned count = size * size;

    make_luma(source, x, y, size, predictor, residuals);
    for (i = 0U; status == REPLAY_OK && i < count; ++i) {
        status = mb_huffman_write(writer, &codec_supermovingblocks_luma_huffman,
                                  residuals[i]);
    }
    if (status != REPLAY_OK) {
        return status;
    }
    for (i = 0U; i < count; ++i) {
        MbPixel *out = &recon[(size_t)(i / size) * recon_stride + i % size];

        out->y = source->pixels[(size_t)(y + i / size) * source->stride +
                                x + i % size].y;
        out->u = u;
        out->v = v;
    }
    return REPLAY_OK;
}

/* "old" data coder: opcode + direct six-bit U, V (14-bit header). */
static ReplayStatus codec20_encode_data_old(ReplayBitWriter *writer,
                                            const MbFrame *source, unsigned x,
                                            unsigned y, unsigned size,
                                            MbPixel *recon, size_t recon_stride,
                                            MbPredictor *predictor)
{
    uint8_t u = mb_encode_average_chroma(source, x, y, size, 0, 32);
    uint8_t v = mb_encode_average_chroma(source, x, y, size, 1, 32);
    uint32_t header = (size == 4U ? UINT32_C(1) : UINT32_C(2)) |
                      ((uint32_t)u << 2U) | ((uint32_t)v << 8U);
    ReplayStatus status = replay_bitwriter_write(writer, header, 14U);

    if (status != REPLAY_OK) {
        return status;
    }
    return write_luma_recon(writer, source, x, y, size, recon, recon_stride,
                            predictor, u, v);
}

/* "new" data coder: opcode + uv-delta byte (10-bit header), chroma predicted. */
static ReplayStatus codec20_encode_data_new(ReplayBitWriter *writer,
                                            const MbFrame *source, unsigned x,
                                            unsigned y, unsigned size,
                                            MbPixel *recon, size_t recon_stride,
                                            MbPredictor *predictor)
{
    uint8_t target_u = mb_encode_average_chroma(source, x, y, size, 0, 32);
    uint8_t target_v = mb_encode_average_chroma(source, x, y, size, 1, 32);
    unsigned u_code = choose_delta(target_u, &predictor->chroma_u);
    unsigned v_code = choose_delta(target_v, &predictor->chroma_v);
    uint32_t header = (size == 4U ? UINT32_C(1) : UINT32_C(2)) |
                      (((u_code & 15U) | ((v_code & 15U) << 4U)) << 2U);
    ReplayStatus status = replay_bitwriter_write(writer, header, 10U);

    if (status != REPLAY_OK) {
        return status;
    }
    return write_luma_recon(writer, source, x, y, size, recon, recon_stride,
                            predictor, predictor->chroma_u, predictor->chroma_v);
}

static const MbEncodeDataCodec codec20_data_old = { codec20_encode_data_old };
static const MbEncodeDataCodec codec20_data_new = { codec20_encode_data_new };

ReplayStatus codec_movingblocksbeta_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksBetaEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksBetaEncodeStats *stats)
{
    MbEncodeOptions encode_options;
    MbEncodeStats encode_stats;
    ReplayStatus status;

    if (options == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    encode_options.allow_stationary = options->allow_stationary != 0;
    encode_options.allow_temporal = options->allow_temporal != 0;
    encode_options.allow_spatial = options->allow_spatial != 0;
    encode_options.allow_split = options->allow_split != 0;
    encode_options.loss_level = options->loss_level;
    encode_options.policy = options->policy;
    encode_options.workspace = options->workspace;

    status = mb_encode_frame(
        &codec20_encode,
        options->variant == CODEC_MOVINGBLOCKSBETA_NEW ? &codec20_data_new
                                                       : &codec20_data_old,
        source, previous, &encode_options, output, reconstructed,
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
        stats->bits_written = encode_stats.bits_written;
    }
    return status;
}
