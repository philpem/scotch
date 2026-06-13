#include "replay/codec_movingblocks.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "replay/mb_codec.h"
#include "replay/mb_encode.h"
#include "replay/mb_motion.h"
#include "replay/mb_quality.h"
#include "replay/replay_bitstream.h"
#include "replay/replay_buffer.h"

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

/*
 * Decode a move case: read the type 7 motion code, validate the reference, and
 * copy a size x size block. Temporal copies take the previous frame, spatial
 * copies the reconstruction in progress (whose legal vectors point at already
 * decoded pixels).
 */
static ReplayStatus apply_move(ReplayBitReader *reader, MbFrame *decoded,
                               const MbFrame *previous, unsigned x, unsigned y,
                               unsigned size, MbVerifyError *error)
{
    MbMotionVector motion;
    const MbFrame *reference;
    int source_x;
    int source_y;
    ReplayStatus status = mb_motion_read_format7(
        reader, size == 4U ? MB_MOTION_BLOCK_4X4 : MB_MOTION_BLOCK_2X2,
        &motion);

    if (status != REPLAY_OK) {
        set_error(error, reader, "invalid or truncated motion code");
        return status;
    }
    reference = motion.spatial != 0 ? decoded : previous;
    if (reference == NULL || reference->pixels == NULL) {
        set_error(error, reader, "temporal copy requires a previous frame");
        return REPLAY_MALFORMED_STREAM;
    }
    source_x = (int)x + motion.dx;
    source_y = (int)y + motion.dy;
    if (source_x < 0 || source_y < 0 ||
        source_x + (int)size > (int)decoded->width ||
        source_y + (int)size > (int)decoded->height) {
        set_error(error, reader, "motion reference lies outside the frame");
        return REPLAY_MALFORMED_STREAM;
    }
    mb_encode_copy_motion(reference, decoded, x, y, size, &motion);
    return REPLAY_OK;
}

/* Verify the four 2x2 children of a split (top-level `01`) block. */
static ReplayStatus verify_split(ReplayBitReader *reader, MbFrame *decoded,
                                 const MbFrame *previous, unsigned x,
                                 unsigned y, MbVerifyError *error)
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
            /* `0` is a 2x2 move case. */
            status = apply_move(reader, decoded, previous, block_x, block_y,
                                2U, error);
            if (status != REPLAY_OK) {
                return status;
            }
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
                    /* `00` is a 4x4 move case. */
                    status = apply_move(&reader, decoded, previous, x, y, 4U,
                                        error);
                } else {
                    status = verify_split(&reader, decoded, previous, x, y,
                                          error);
                }
            }
        }
    }
    if (status == REPLAY_OK && bits_consumed != NULL) {
        *bits_consumed = replay_bitreader_position(&reader);
    }
    return status;
}

/* ---------------------------------------------------------------------- *
 * Encoder.
 *
 * The motion search and copy/quality model are shared with the family, so the
 * type 7 encoder reuses mb_encode's selectors and copy helpers. What is type 7
 * specific is the grammar emission -- `1` data, `00`/`0` move, `01` split, with
 * no distinct stationary opcode -- the +/-4 motion write, and literal data.
 * ---------------------------------------------------------------------- */

static int codec7_temporal_vector(unsigned index, MbMotionVector *out)
{
    return mb_motion_format7_temporal_at(index, out) == REPLAY_OK;
}

static int codec7_spatial_vector(MbMotionBlockSize block_size, unsigned index,
                                 MbMotionVector *out)
{
    return mb_motion_format7_spatial_at(block_size, index, out) == REPLAY_OK;
}

static int codec7_block_match(const MbFrame *source, unsigned x, unsigned y,
                              const MbFrame *reference, unsigned ref_x,
                              unsigned ref_y, unsigned size, uint8_t u,
                              uint8_t v, const void *quality, unsigned *error)
{
    return mb_quality_match_format19(source, x, y, reference, ref_x, ref_y,
                                     size, u, v,
                                     (const MbQualityThresholds *)quality,
                                     error);
}

static int codec7_profile_match(const MbFrame *source, unsigned x, unsigned y,
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

/*
 * Type 7 shares the 5-bit YUV555 quality metric with type 17 (the absolute
 * error metric is indifferent to the 5-vs-6-bit luma depth), so the quality
 * hooks reuse the format-19 functions; only the vector tables are type 7's.
 */
static const MbEncodeCodec codec7_encode = {
    MB_QUALITY_LEVEL_COUNT,
    80U,
    8U,
    codec7_temporal_vector,
    codec7_spatial_vector,
    codec7_block_match,
    codec7_profile_match
};

/*
 * Write a literal data block (no opcode): the row-major 5-bit luma values then
 * the shared 5-bit U and V. Luma is lossless; chroma is the block average. The
 * reconstruction is written compactly at recon_stride.
 */
static ReplayStatus encode_data_literal(ReplayBitWriter *writer,
                                        const MbFrame *source, unsigned x,
                                        unsigned y, unsigned size,
                                        MbPixel *recon, size_t recon_stride)
{
    uint8_t u = mb_encode_average_chroma(source, x, y, size, 0);
    uint8_t v = mb_encode_average_chroma(source, x, y, size, 1);
    unsigned row;
    ReplayStatus status = REPLAY_OK;

    for (row = 0U; status == REPLAY_OK && row < size; ++row) {
        unsigned column;

        for (column = 0U; status == REPLAY_OK && column < size; ++column) {
            const MbPixel *p =
                &source->pixels[(size_t)(y + row) * source->stride + x + column];
            MbPixel *out = &recon[(size_t)row * recon_stride + column];

            status = replay_bitwriter_write(writer, p->y, 5U);
            out->y = p->y;
            out->u = u;
            out->v = v;
        }
    }
    if (status == REPLAY_OK) {
        status = replay_bitwriter_write(writer, u, 5U);
    }
    if (status == REPLAY_OK) {
        status = replay_bitwriter_write(writer, v, 5U);
    }
    return status;
}

/* Build a `1` + literal 4x4 data candidate into a scratch buffer. */
static ReplayStatus build_data7(const MbFrame *source, unsigned x, unsigned y,
                                ReplayBuffer *buffer, MbPixel recon[16],
                                size_t *bits)
{
    ReplayBitWriter writer;
    ReplayStatus status;

    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    status = replay_bitwriter_write(&writer, 1U, 1U); /* `1` data 4x4 */
    if (status == REPLAY_OK) {
        status = encode_data_literal(&writer, source, x, y, 4U, recon, 4U);
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Emit a move: the chosen vector's format 7 code (the caller wrote the opcode),
 * then reconstruct the copy. Stationary folds into a zero temporal vector. */
static ReplayStatus emit_move(ReplayBitWriter *writer, SplitMode mode,
                              const MbMotionVector *motion,
                              MbMotionBlockSize block_size)
{
    MbMotionVector vector =
        mode == SPLIT_MODE_STATIONARY ? (MbMotionVector){ 0, 0, 0 } : *motion;

    return mb_motion_write_format7(writer, block_size, &vector);
}

/*
 * Build a `01` split candidate: four 2x2 children, each a `1` data or `0` move,
 * tracking the tentative reconstruction so a later spatial child can reference
 * earlier ones. Mirrors mb_encode's 17/19 split builder with type 7 emission.
 */
static ReplayStatus build_split7(const MbFrame *source, const MbFrame *previous,
                                 const MbFrame *reconstructed, unsigned x,
                                 unsigned y, int allow_stationary,
                                 int allow_temporal, int allow_spatial,
                                 const MbQualityThresholds *quality,
                                 unsigned loss_level, MbEncodePolicy policy,
                                 ReplayBuffer *buffer, MbPixel tentative[16],
                                 SplitMode modes[4], size_t *bits,
                                 MbEncodeStats *stats,
                                 MbEncodeWorkspace *workspace)
{
    static const unsigned offsets[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    ReplayBitWriter writer;
    unsigned available_mask = 0U;
    unsigned block;
    ReplayStatus status;

    for (block = 0U; block < 16U; ++block) {
        tentative[block] = (MbPixel){ 0U, 0U, 0U };
    }
    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    status = replay_bitwriter_write(&writer, 2U, 2U); /* `01` split */
    for (block = 0U; status == REPLAY_OK && block < 4U; ++block) {
        unsigned block_x = x + offsets[block][0];
        unsigned block_y = y + offsets[block][1];
        unsigned local = (block_y - y) * 4U + block_x - x;
        uint8_t u = mb_encode_average_chroma(source, block_x, block_y, 2U, 0);
        uint8_t v = mb_encode_average_chroma(source, block_x, block_y, 2U, 1);
        CopyCandidate selected = mb_encode_select_copy2x2(
            &codec7_encode, source, previous, reconstructed, tentative,
            available_mask, x, y, block_x, block_y, u, v, allow_stationary,
            allow_temporal, allow_spatial, quality, loss_level, policy, stats,
            workspace);

        modes[block] = selected.mode;
        if (selected.mode == SPLIT_MODE_DATA) {
            status = replay_bitwriter_write(&writer, 1U, 1U); /* `1` data */
            if (status == REPLAY_OK) {
                status = encode_data_literal(&writer, source, block_x, block_y,
                                             2U, &tentative[local], 4U);
            }
            available_mask |= 1U << local;
            available_mask |= 1U << (local + 1U);
            available_mask |= 1U << (local + 4U);
            available_mask |= 1U << (local + 5U);
        } else {
            const MbFrame *reference =
                selected.mode == SPLIT_MODE_SPATIAL ? reconstructed : previous;

            status = replay_bitwriter_write(&writer, 0U, 1U); /* `0` move */
            if (status == REPLAY_OK) {
                status = emit_move(&writer, selected.mode, &selected.motion,
                                   MB_MOTION_BLOCK_2X2);
            }
            mb_encode_fill_tentative_copy2x2(
                reference, reconstructed, tentative, x, y, block_x, block_y,
                selected.mode == SPLIT_MODE_STATIONARY
                    ? &(MbMotionVector){ 0, 0, 0 }
                    : &selected.motion,
                &available_mask);
        }
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Write a compact 16-pixel reconstruction block into the frame. */
static void blit_block4x4(MbFrame *destination, unsigned x, unsigned y,
                          const MbPixel block[16])
{
    unsigned row;

    for (row = 0U; row < 4U; ++row) {
        unsigned column;

        for (column = 0U; column < 4U; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] = block[row * 4U + column];
        }
    }
}

static void tally_split_modes(const SplitMode modes[4], MbEncodeStats *stats)
{
    unsigned block;

    for (block = 0U; block < 4U; ++block) {
        switch (modes[block]) {
        case SPLIT_MODE_STATIONARY:
            ++stats->stationary2x2_blocks;
            break;
        case SPLIT_MODE_TEMPORAL:
            ++stats->temporal2x2_blocks;
            break;
        case SPLIT_MODE_SPATIAL:
            ++stats->spatial2x2_blocks;
            break;
        default:
            ++stats->data2x2_blocks;
            break;
        }
    }
}

static int frame_pair_ok(const MbFrame *source, const MbFrame *reconstructed)
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

ReplayStatus codec_movingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksEncodeStats *stats)
{
    ReplayBitWriter writer;
    ReplayBuffer data_candidate;
    ReplayBuffer split_candidate;
    MbEncodeStats local_stats = { 0 };
    MbQualityThresholds quality;
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    MbEncodePolicy policy;
    MbEncodeWorkspace *workspace;
    unsigned loss_level;
    unsigned y;
    ReplayStatus status = REPLAY_OK;

    if (options == NULL || output == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    allow_stationary = options->allow_stationary != 0;
    allow_temporal = options->allow_temporal != 0;
    allow_spatial = options->allow_spatial != 0;
    allow_split = options->allow_split != 0;
    policy = options->policy;
    workspace = options->workspace;
    loss_level = options->loss_level;

    replay_buffer_clear(output);
    if ((policy != MB_ENCODE_POLICY_ORDERED &&
         policy != MB_ENCODE_POLICY_LOWEST_ERROR) ||
        mb_quality_thresholds(loss_level, &quality) != REPLAY_OK ||
        !frame_pair_ok(source, reconstructed) ||
        (workspace != NULL &&
         (workspace->width != source->width ||
          workspace->height != source->height)) ||
        ((allow_stationary || allow_temporal) &&
         (previous == NULL || previous->pixels == NULL ||
          previous->width != source->width ||
          previous->height != source->height ||
          previous->stride < previous->width))) {
        return REPLAY_INVALID_ARGUMENT;
    }

    replay_bitwriter_init(&writer, output);
    replay_buffer_init(&data_candidate);
    replay_buffer_init(&split_candidate);
    for (y = 0U; status == REPLAY_OK && y < source->height; y += 4U) {
        unsigned x;

        for (x = 0U; status == REPLAY_OK && x < source->width; x += 4U) {
            uint8_t u = mb_encode_average_chroma(source, x, y, 4U, 0);
            uint8_t v = mb_encode_average_chroma(source, x, y, 4U, 1);
            CopyCandidate copy = mb_encode_select_copy4x4(
                &codec7_encode, source, previous, reconstructed, x, y, u, v,
                allow_stationary, allow_temporal, allow_spatial, &quality,
                loss_level, policy, &local_stats, workspace);

            if (copy.mode != SPLIT_MODE_DATA) {
                const MbFrame *reference =
                    copy.mode == SPLIT_MODE_SPATIAL ? reconstructed : previous;
                MbMotionVector vector =
                    copy.mode == SPLIT_MODE_STATIONARY
                        ? (MbMotionVector){ 0, 0, 0 }
                        : copy.motion;

                status = replay_bitwriter_write(&writer, 0U, 2U); /* `00` */
                if (status == REPLAY_OK) {
                    status = emit_move(&writer, copy.mode, &copy.motion,
                                       MB_MOTION_BLOCK_4X4);
                }
                mb_encode_copy_motion(reference, reconstructed, x, y, 4U,
                                      &vector);
                if (copy.mode == SPLIT_MODE_STATIONARY) {
                    ++local_stats.stationary4x4_blocks;
                } else if (copy.mode == SPLIT_MODE_TEMPORAL) {
                    ++local_stats.temporal4x4_blocks;
                } else {
                    ++local_stats.spatial4x4_blocks;
                }
            } else {
                MbPixel data_recon[16];
                MbPixel split_recon[16];
                SplitMode split_modes[4];
                size_t data_bits;
                size_t split_bits = SIZE_MAX;

                status = build_data7(source, x, y, &data_candidate, data_recon,
                                     &data_bits);
                if (status == REPLAY_OK && allow_split) {
                    status = build_split7(
                        source, previous, reconstructed, x, y, allow_stationary,
                        allow_temporal, allow_spatial, &quality, loss_level,
                        policy, &split_candidate, split_recon, split_modes,
                        &split_bits, &local_stats, workspace);
                }
                if (status == REPLAY_OK && split_bits < data_bits) {
                    status = mb_encode_append_candidate(&writer, &split_candidate,
                                                        split_bits);
                    blit_block4x4(reconstructed, x, y, split_recon);
                    tally_split_modes(split_modes, &local_stats);
                    ++local_stats.split4x4_blocks;
                } else if (status == REPLAY_OK) {
                    status = mb_encode_append_candidate(&writer, &data_candidate,
                                                        data_bits);
                    blit_block4x4(reconstructed, x, y, data_recon);
                    ++local_stats.data4x4_blocks;
                }
            }
        }
    }

    if (status == REPLAY_OK) {
        local_stats.bits_written = replay_bitwriter_position(&writer);
        status = replay_bitwriter_flush_zero(&writer);
    }
    replay_buffer_free(&split_candidate);
    replay_buffer_free(&data_candidate);
    if (status != REPLAY_OK) {
        replay_buffer_clear(output);
        return status;
    }
    if (stats != NULL) {
        stats->data4x4_blocks = local_stats.data4x4_blocks;
        stats->stationary4x4_blocks = local_stats.stationary4x4_blocks;
        stats->temporal4x4_blocks = local_stats.temporal4x4_blocks;
        stats->spatial4x4_blocks = local_stats.spatial4x4_blocks;
        stats->split4x4_blocks = local_stats.split4x4_blocks;
        stats->data2x2_blocks = local_stats.data2x2_blocks;
        stats->stationary2x2_blocks = local_stats.stationary2x2_blocks;
        stats->temporal2x2_blocks = local_stats.temporal2x2_blocks;
        stats->spatial2x2_blocks = local_stats.spatial2x2_blocks;
        stats->bits_written = local_stats.bits_written;
    }
    return REPLAY_OK;
}
