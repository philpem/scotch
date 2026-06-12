#include "replay/codec_supermovingblocks.h"
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

static int signed_chroma(uint8_t value)
{
    /* Five-bit chroma is stored as two's-complement modulo 32. */
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
    /* Match the source assembler's arithmetic shift, including negatives. */
    if (sum < 0) {
        sum = -((-sum + 15) / 16);
    } else {
        sum /= 16;
    }
    return (uint8_t)(sum & 31);
}

static uint8_t average_chroma2x2(const MbFrame *source, unsigned x,
                                 unsigned y, int use_v)
{
    int sum = 0;
    unsigned row;

    for (row = 0; row < 2U; ++row) {
        unsigned column;
        for (column = 0; column < 2U; ++column) {
            const MbPixel *pixel =
                &source->pixels[(size_t)(y + row) * source->stride +
                                x + column];
            sum += signed_chroma(use_v != 0 ? pixel->v : pixel->u);
        }
    }
    if (sum < 0) {
        sum = -((-sum + 3) / 4);
    } else {
        sum /= 4;
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

static void reconstruct_data4x4(const MbFrame *source, MbFrame *reconstructed,
                                unsigned x, unsigned y, uint8_t u, uint8_t v)
{
    unsigned row;

    /*
     * Luma data is lossless at its six-bit working precision. Chroma is not:
     * the decoder expands the one U/V pair from the block header to all 16
     * pixels. This reconstructed form, not the unaveraged source, is the
     * reference for future inter-frame decisions.
     */
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

static void reconstruct_data2x2(const MbFrame *source, MbFrame *reconstructed,
                                unsigned x, unsigned y, uint8_t u, uint8_t v)
{
    unsigned row;

    for (row = 0; row < 2U; ++row) {
        unsigned column;
        for (column = 0; column < 2U; ++column) {
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

static ReplayStatus append_candidate(ReplayBitWriter *writer,
                                     const ReplayBuffer *buffer, size_t bits)
{
    ReplayBitReader reader;

    /*
     * Candidate buffers are flushed to whole bytes for simple ownership, but
     * only `bits` meaningful bits belong to the stream. Re-reading that exact
     * count prevents a candidate's temporary zero padding becoming an
     * accidental gap before the next block.
     */
    replay_bitreader_init(&reader, buffer->data, buffer->size);
    while (bits != 0U) {
        unsigned count = bits > 32U ? 32U : (unsigned)bits;
        uint32_t value;
        ReplayStatus status = replay_bitreader_read(&reader, count, &value);

        if (status != REPLAY_OK) {
            return status;
        }
        status = replay_bitwriter_write(writer, value, count);
        if (status != REPLAY_OK) {
            return status;
        }
        bits -= count;
    }
    return REPLAY_OK;
}

static ReplayStatus build_data4x4_candidate(
    const MbFrame *source, unsigned x, unsigned y,
    const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPredictor *final_predictor, size_t *bits)
{
    ReplayBitWriter writer;
    uint8_t residuals[16];
    uint8_t u = average_chroma4x4(source, x, y, 0);
    uint8_t v = average_chroma4x4(source, x, y, 1);
    ReplayStatus status;

    /* Work from a predictor copy: considering a candidate must have no side effects. */
    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    *final_predictor = *initial_predictor;
    status = make_data4x4(source, x, y, final_predictor, residuals);
    if (status == REPLAY_OK) {
        status = codec_supermovingblocks_write_data4x4(
            &writer, u, v, residuals);
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

typedef enum {
    SPLIT_MODE_DATA,
    SPLIT_MODE_STATIONARY,
    SPLIT_MODE_TEMPORAL,
    SPLIT_MODE_SPATIAL
} SplitMode;

typedef struct {
    SplitMode mode;
    MbMotionVector motion;
} SplitDecision;

typedef struct {
    SplitMode mode;
    MbMotionVector motion;
    unsigned error;
    unsigned bits;
    unsigned order;
    int valid;
} CopyCandidate;

typedef struct {
    int8_t dx;
    int8_t dy;
    uint16_t error;
    uint8_t valid;
} TemporalResult;

typedef struct {
    TemporalResult levels[MB_QUALITY_LEVEL_COUNT];
    uint8_t ready;
} TemporalCacheEntry;

typedef struct {
    unsigned width;
    unsigned height;
    size_t count4x4;
    size_t count2x2;
    TemporalCacheEntry *entries4x4;
    TemporalCacheEntry *entries2x2;
} SuperMovingBlocksWorkspaceInternal;

static unsigned first_accepted_level(const MbQualityProfile *profile,
                                     unsigned block_size)
{
    unsigned low = 0U;
    unsigned high = MB_QUALITY_LEVEL_COUNT - 1U;
    MbQualityThresholds thresholds;
    unsigned error;

    /* The source quality table loosens monotonically from level 0 through 28. */
    (void)mb_quality_thresholds(high, &thresholds);
    if (!mb_quality_profile_accept(
            profile, block_size, &thresholds, &error)) {
        return MB_QUALITY_LEVEL_COUNT;
    }
    while (low < high) {
        unsigned middle = low + (high - low) / 2U;

        (void)mb_quality_thresholds(middle, &thresholds);
        if (mb_quality_profile_accept(
                profile, block_size, &thresholds, &error)) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return low;
}

static SuperMovingBlocksWorkspaceInternal *workspace_internal(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    return workspace != NULL
               ? (SuperMovingBlocksWorkspaceInternal *)workspace->internal
               : NULL;
}

ReplayStatus codec_supermovingblocks_workspace_init(
    CodecSuperMovingBlocksWorkspace *workspace, unsigned width,
    unsigned height)
{
    SuperMovingBlocksWorkspaceInternal *internal;
    size_t count4x4;
    size_t count2x2;

    if (workspace == NULL || workspace->internal != NULL || width == 0U ||
        height == 0U || (width & 3U) != 0U || (height & 3U) != 0U ||
        (size_t)(width / 4U) > SIZE_MAX / (height / 4U) ||
        (size_t)(width / 2U) > SIZE_MAX / (height / 2U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    count4x4 = (size_t)(width / 4U) * (height / 4U);
    count2x2 = (size_t)(width / 2U) * (height / 2U);
    internal = calloc(1U, sizeof(*internal));
    if (internal == NULL) {
        return REPLAY_OUT_OF_MEMORY;
    }
    internal->entries4x4 = calloc(count4x4, sizeof(*internal->entries4x4));
    internal->entries2x2 = calloc(count2x2, sizeof(*internal->entries2x2));
    if (internal->entries4x4 == NULL || internal->entries2x2 == NULL) {
        free(internal->entries2x2);
        free(internal->entries4x4);
        free(internal);
        return REPLAY_OUT_OF_MEMORY;
    }
    internal->width = width;
    internal->height = height;
    internal->count4x4 = count4x4;
    internal->count2x2 = count2x2;
    workspace->internal = internal;
    return REPLAY_OK;
}

void codec_supermovingblocks_workspace_reset(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    SuperMovingBlocksWorkspaceInternal *internal = workspace_internal(workspace);

    if (internal != NULL) {
        memset(internal->entries4x4, 0,
               internal->count4x4 * sizeof(*internal->entries4x4));
        memset(internal->entries2x2, 0,
               internal->count2x2 * sizeof(*internal->entries2x2));
    }
}

void codec_supermovingblocks_workspace_destroy(
    CodecSuperMovingBlocksWorkspace *workspace)
{
    SuperMovingBlocksWorkspaceInternal *internal = workspace_internal(workspace);

    if (internal != NULL) {
        free(internal->entries2x2);
        free(internal->entries4x4);
        free(internal);
        workspace->internal = NULL;
    }
}

static unsigned motion_bits(const MbMotionVector *motion,
                            MbMotionBlockSize block_size)
{
    unsigned magnitude;
    unsigned body_bits;

    if (motion->spatial != 0) {
        body_bits = 7U;
    } else {
        magnitude = (unsigned)(motion->dx < 0 ? -motion->dx : motion->dx);
        if ((unsigned)(motion->dy < 0 ? -motion->dy : motion->dy) > magnitude) {
            magnitude = (unsigned)(motion->dy < 0 ? -motion->dy : motion->dy);
        }
        body_bits = magnitude <= 1U ? 5U
                  : magnitude == 2U ? 6U
                  : magnitude == 3U ? 7U
                                    : 10U;
    }
    return body_bits + (block_size == MB_MOTION_BLOCK_4X4 ? 2U : 1U);
}

static int candidate_better(const CopyCandidate *candidate,
                            const CopyCandidate *best)
{
    return candidate->valid &&
           (!best->valid || candidate->error < best->error ||
            (candidate->error == best->error &&
             (candidate->bits < best->bits ||
              (candidate->bits == best->bits &&
               candidate->order < best->order))));
}

/*
 * Search the same temporal vector sequence used by 4x4 blocks, but compare a
 * 2x2 reconstruction target. Lowest error wins; enumeration order gives the
 * shortest/source-first code when candidates have equal error.
 */
static int match_temporal2x2(const MbFrame *source, const MbFrame *previous,
                             unsigned x, unsigned y, uint8_t u, uint8_t v,
                             unsigned loss_level,
                             MbMotionVector *motion, unsigned *matched_error,
                             size_t *evaluations,
                             SuperMovingBlocksWorkspaceInternal *workspace)
{
    TemporalCacheEntry local_entry = { { { 0 } }, 0U };
    TemporalCacheEntry *entry = &local_entry;
    unsigned index;

    if (workspace != NULL) {
        size_t cache_index = (size_t)(y / 2U) * (workspace->width / 2U) +
                             x / 2U;
        entry = &workspace->entries2x2[cache_index];
    }
    if (entry->ready) {
        const TemporalResult *result = &entry->levels[loss_level];
        if (!result->valid) {
            return 0;
        }
        *motion = (MbMotionVector){ result->dx, result->dy, 0 };
        *matched_error = result->error;
        return 1;
    }

    for (index = 0U; index < 288U; ++index) {
        MbMotionVector candidate;
        MbQualityProfile profile;
        int source_x;
        int source_y;
        unsigned level;

        if (mb_motion_format19_temporal_at(index, &candidate) != REPLAY_OK) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 2 > (int)previous->width ||
            source_y + 2 > (int)previous->height) {
            continue;
        }
        ++*evaluations;
        if (!mb_quality_profile_format19(
                source, x, y, previous, (unsigned)source_x,
                (unsigned)source_y, 2U, u, v, &profile)) {
            continue;
        }
        level = first_accepted_level(&profile, 2U);
        for (; level < MB_QUALITY_LEVEL_COUNT; ++level) {
            TemporalResult *result = &entry->levels[level];

            if (!result->valid || profile.total_error < result->error) {
                result->dx = (int8_t)candidate.dx;
                result->dy = (int8_t)candidate.dy;
                result->error = profile.total_error;
                result->valid = 1U;
            }
        }
        /* Zero error is optimal at every quality row and table-order tie. */
        if (profile.total_error == 0U) {
            break;
        }
    }
    entry->ready = 1U;
    if (!entry->levels[loss_level].valid) {
        return 0;
    }
    *motion = (MbMotionVector){ entry->levels[loss_level].dx,
                               entry->levels[loss_level].dy, 0 };
    *matched_error = entry->levels[loss_level].error;
    return 1;
}

static const MbPixel *split_spatial_pixel(
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned block_x, unsigned block_y,
    unsigned source_x, unsigned source_y)
{
    /*
     * A spatial source can fall inside the 4x4 block currently being tested.
     * In that case it is legal only if an earlier 2x2 decision has already
     * produced the pixel in the private tentative reconstruction.
     */
    if (source_x >= block_x && source_x < block_x + 4U &&
        source_y >= block_y && source_y < block_y + 4U) {
        unsigned local = (source_y - block_y) * 4U + source_x - block_x;
        return (available_mask & (1U << local)) != 0U
                   ? &tentative[local]
                   : NULL;
    }

    /*
     * Top-level blocks are reconstructed as complete 4x4 units in raster
     * order. A 2x2 vector can point upward and right; near the right edge of
     * the current parent that rectangle may cross into the next 4x4 block,
     * whose pixels do not exist yet. Reading the live frame there observes
     * stale buffer contents and can make an invalid match appear acceptable.
     *
     * Pixels outside the tentative parent are therefore available only when
     * their owning top-level block precedes the current parent in codec scan
     * order. Pixels inside the parent are governed by available_mask above.
     */
    {
        unsigned source_block_x = source_x & ~3U;
        unsigned source_block_y = source_y & ~3U;

        if (source_block_y > block_y ||
            (source_block_y == block_y && source_block_x >= block_x)) {
            return NULL;
        }
    }
    return &reconstructed->pixels[(size_t)source_y * reconstructed->stride +
                                  source_x];
}

static int match_spatial2x2(const MbFrame *source,
                            const MbFrame *reconstructed,
                            const MbPixel tentative[16],
                            unsigned available_mask, unsigned parent_x,
                            unsigned parent_y, unsigned x, unsigned y,
                            uint8_t u, uint8_t v,
                            const MbQualityThresholds *quality,
                            MbMotionVector *motion, unsigned *matched_error,
                            size_t *evaluations)
{
    unsigned best_error = UINT32_MAX;
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        MbMotionVector candidate;
        MbPixel candidate_pixels[4];
        MbFrame candidate_frame = { 2U, 2U, 2U, candidate_pixels };
        int source_x;
        int source_y;
        unsigned row;
        unsigned error;

        if (mb_motion_format19_spatial_at(
                MB_MOTION_BLOCK_2X2, index, &candidate) != REPLAY_OK) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 2 > (int)reconstructed->width ||
            source_y + 2 > (int)reconstructed->height) {
            continue;
        }
        for (row = 0U; row < 2U; ++row) {
            unsigned column;
            for (column = 0U; column < 2U; ++column) {
                const MbPixel *reference = split_spatial_pixel(
                    reconstructed, tentative, available_mask,
                    parent_x, parent_y, (unsigned)source_x + column,
                    (unsigned)source_y + row);
                if (reference == NULL) {
                    break;
                }
                candidate_pixels[row * 2U + column] = *reference;
            }
            if (column != 2U) {
                break;
            }
        }
        if (row == 2U) {
            ++*evaluations;
        }
        if (row == 2U && mb_quality_match_format19(
                             source, x, y, &candidate_frame, 0U, 0U, 2U,
                             u, v, quality, &error) &&
            error < best_error) {
            *motion = candidate;
            best_error = error;
            /* All spatial codes have equal length; table order breaks ties. */
            if (error == 0U) {
                break;
            }
        }
    }
    if (best_error == UINT32_MAX) {
        return 0;
    }
    *matched_error = best_error;
    return 1;
}

static CopyCandidate select_copy2x2(
    const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, uint8_t u, uint8_t v, int allow_stationary,
    int allow_temporal, int allow_spatial,
    const MbQualityThresholds *quality, unsigned loss_level,
    CodecSuperMovingBlocksPolicy policy,
    CodecSuperMovingBlocksEncodeStats *stats,
    SuperMovingBlocksWorkspaceInternal *workspace)
{
    CopyCandidate selected = {
        SPLIT_MODE_DATA, { 0, 0, 0 }, 0U, 0U, 0U, 0
    };
    CopyCandidate candidate;
    unsigned error;

    if (allow_stationary) {
        ++stats->stationary2x2_evaluations;
    }
    if (allow_stationary && mb_quality_match_format19(
            source, x, y, previous, x, y, 2U, u, v, quality, &error)) {
        selected = (CopyCandidate){
            SPLIT_MODE_STATIONARY, { 0, 0, 0 }, error, 2U, 0U, 1
        };
        /* Exact stationary is minimal in both distortion and encoded bits. */
        if (policy == CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED || error == 0U) {
            return selected;
        }
    }

    candidate = (CopyCandidate){
        SPLIT_MODE_TEMPORAL, { 0, 0, 0 }, 0U, 0U, 1U, 0
    };
    if (allow_temporal && match_temporal2x2(
            source, previous, x, y, u, v, loss_level,
            &candidate.motion, &error,
            &stats->temporal2x2_evaluations, workspace)) {
        candidate.error = error;
        candidate.bits = motion_bits(&candidate.motion, MB_MOTION_BLOCK_2X2);
        candidate.valid = 1;
        if (policy == CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED) {
            return candidate;
        }
        if (candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }

    candidate = (CopyCandidate){
        SPLIT_MODE_SPATIAL, { 0, 0, 1 }, 0U, 0U, 2U, 0
    };
    if (allow_spatial && match_spatial2x2(
            source, reconstructed, tentative, available_mask,
            parent_x, parent_y, x, y, u, v, quality,
            &candidate.motion, &error,
            &stats->spatial2x2_evaluations)) {
        candidate.error = error;
        candidate.bits = motion_bits(&candidate.motion, MB_MOTION_BLOCK_2X2);
        candidate.valid = 1;
        if (candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }
    return selected;
}

static void fill_tentative_data2x2(const MbFrame *source,
                                   MbPixel tentative[16], unsigned parent_x,
                                   unsigned parent_y, unsigned x, unsigned y,
                                   uint8_t u, uint8_t v,
                                   unsigned *available_mask)
{
    unsigned row;

    for (row = 0U; row < 2U; ++row) {
        unsigned column;
        for (column = 0U; column < 2U; ++column) {
            unsigned local = (y + row - parent_y) * 4U + x + column - parent_x;
            tentative[local].y =
                source->pixels[(size_t)(y + row) * source->stride +
                               x + column].y;
            tentative[local].u = u;
            tentative[local].v = v;
            *available_mask |= 1U << local;
        }
    }
}

static void fill_tentative_copy2x2(
    const MbFrame *reference, const MbFrame *reconstructed,
    MbPixel tentative[16], unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, const MbMotionVector *motion,
    unsigned *available_mask)
{
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);
    unsigned row;

    for (row = 0U; row < 2U; ++row) {
        unsigned column;
        for (column = 0U; column < 2U; ++column) {
            unsigned local = (y + row - parent_y) * 4U + x + column - parent_x;
            const MbPixel *pixel;

            if (motion->spatial != 0) {
                pixel = split_spatial_pixel(
                    reconstructed, tentative, *available_mask,
                    parent_x, parent_y, source_x + column, source_y + row);
            } else {
                pixel = &reference->pixels[(size_t)(source_y + row) *
                                               reference->stride +
                                           source_x + column];
            }
            tentative[local] = *pixel;
            *available_mask |= 1U << local;
        }
    }
}

static ReplayStatus build_split_data_candidate(
    const MbFrame *source, unsigned x, unsigned y,
    const MbFrame *previous, const MbFrame *reconstructed,
    int allow_stationary, int allow_temporal, int allow_spatial,
    CodecSuperMovingBlocksPolicy policy,
    const MbQualityThresholds *quality, unsigned loss_level,
    const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPredictor *final_predictor, SplitDecision decisions[4], size_t *bits,
    CodecSuperMovingBlocksEncodeStats *stats,
    SuperMovingBlocksWorkspaceInternal *workspace)
{
    /* Decoder order inside a split block is TL, TR, BL, BR. */
    static const unsigned offsets[4][2] = {
        { 0U, 0U }, { 2U, 0U }, { 0U, 2U }, { 2U, 2U }
    };
    ReplayBitWriter writer;
    /*
     * Candidate evaluation cannot write into the live reconstructed frame:
     * the competing 4x4 data candidate may win. These 16 pixels model exactly
     * what the decoder would have reconstructed after each tentative quadrant.
     */
    MbPixel tentative[16] = { { 0U, 0U, 0U } };
    unsigned available_mask = 0U;
    unsigned block;
    ReplayStatus status;

    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    *final_predictor = *initial_predictor;
    status = replay_bitwriter_write(&writer, UINT32_C(3), 2U);
    for (block = 0U; status == REPLAY_OK && block < 4U; ++block) {
        unsigned block_x = x + offsets[block][0];
        unsigned block_y = y + offsets[block][1];
        uint8_t residuals[4];
        uint8_t u = average_chroma2x2(source, block_x, block_y, 0);
        uint8_t v = average_chroma2x2(source, block_x, block_y, 1);
        /*
         * Compare against the 2x2 data reconstruction target: source luma plus
         * one averaged U/V pair. Comparing raw source chroma would reject a
         * copy that decodes identically to the corresponding data block.
         */
        CopyCandidate selected = select_copy2x2(
            source, previous, reconstructed, tentative, available_mask,
            x, y, block_x, block_y, u, v,
            allow_stationary, allow_temporal, allow_spatial, quality,
            loss_level, policy, stats, workspace);

        decisions[block].mode = selected.mode;
        decisions[block].motion = selected.motion;
        if (selected.mode == SPLIT_MODE_STATIONARY) {
            status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
            fill_tentative_copy2x2(
                previous, reconstructed, tentative, x, y, block_x, block_y,
                &decisions[block].motion, &available_mask);
        } else if (selected.mode == SPLIT_MODE_TEMPORAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(
                    &writer, MB_MOTION_BLOCK_2X2,
                    &decisions[block].motion);
            }
            fill_tentative_copy2x2(
                previous, reconstructed, tentative, x, y, block_x, block_y,
                &decisions[block].motion, &available_mask);
        } else if (selected.mode == SPLIT_MODE_SPATIAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(
                    &writer, MB_MOTION_BLOCK_2X2,
                    &decisions[block].motion);
            }
            fill_tentative_copy2x2(
                reconstructed, reconstructed, tentative, x, y,
                block_x, block_y, &decisions[block].motion,
                &available_mask);
        } else {
            status = make_data2x2(
                source, block_x, block_y, final_predictor, residuals);
            if (status == REPLAY_OK) {
                status = codec_supermovingblocks_write_data2x2(
                    &writer, u, v, residuals);
            }
            fill_tentative_data2x2(
                source, tentative, x, y, block_x, block_y, u, v,
                &available_mask);
        }
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

static int block_matches_data_reconstruction(const MbFrame *source,
                                             const MbFrame *previous,
                                             unsigned x, unsigned y,
                                             uint8_t u, uint8_t v,
                                             const MbQualityThresholds *quality,
                                             unsigned *error)
{
    /* `u` and `v` are the block averages a data block would actually decode. */
    return mb_quality_match_format19(
        source, x, y, previous, x, y, 4U, u, v, quality, error);
}

static int find_temporal4x4(const MbFrame *source, const MbFrame *previous,
                            unsigned x, unsigned y, uint8_t u, uint8_t v,
                            unsigned loss_level,
                            MbMotionVector *motion, unsigned *matched_error,
                            size_t *evaluations,
                            SuperMovingBlocksWorkspaceInternal *workspace)
{
    TemporalCacheEntry local_entry = { { { 0 } }, 0U };
    TemporalCacheEntry *entry = &local_entry;
    unsigned index;

    if (workspace != NULL) {
        size_t cache_index = (size_t)(y / 4U) * (workspace->width / 4U) +
                             x / 4U;
        entry = &workspace->entries4x4[cache_index];
    }
    if (entry->ready) {
        const TemporalResult *result = &entry->levels[loss_level];
        if (!result->valid) {
            return 0;
        }
        *motion = (MbMotionVector){ result->dx, result->dy, 0 };
        *matched_error = result->error;
        return 1;
    }

    /* Enumeration order gives the shortest/source-first equal-error code. */
    for (index = 0U; index < 288U; ++index) {
        MbMotionVector candidate;
        MbQualityProfile profile;
        int source_x;
        int source_y;
        unsigned level;

        if (mb_motion_format19_temporal_at(index, &candidate) != REPLAY_OK) {
            return 0;
        }
        source_x = (int)x + candidate.dx;
        source_y = (int)y + candidate.dy;
        if (source_x < 0 || source_y < 0 ||
            source_x + 4 > (int)previous->width ||
            source_y + 4 > (int)previous->height) {
            continue;
        }
        ++*evaluations;
        if (!mb_quality_profile_format19(
                source, x, y, previous, (unsigned)source_x,
                (unsigned)source_y, 4U, u, v, &profile)) {
            continue;
        }
        level = first_accepted_level(&profile, 4U);
        for (; level < MB_QUALITY_LEVEL_COUNT; ++level) {
            TemporalResult *result = &entry->levels[level];

            if (!result->valid || profile.total_error < result->error) {
                result->dx = (int8_t)candidate.dx;
                result->dy = (int8_t)candidate.dy;
                result->error = profile.total_error;
                result->valid = 1U;
            }
        }
        /* Zero error is optimal at every quality row and table-order tie. */
        if (profile.total_error == 0U) {
            break;
        }
    }
    entry->ready = 1U;
    if (!entry->levels[loss_level].valid) {
        return 0;
    }
    *motion = (MbMotionVector){ entry->levels[loss_level].dx,
                               entry->levels[loss_level].dy, 0 };
    *matched_error = entry->levels[loss_level].error;
    return 1;
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

static int spatial_block_matches(const MbFrame *source,
                                 const MbFrame *reconstructed, unsigned x,
                                 unsigned y, const MbMotionVector *motion,
                                 uint8_t u, uint8_t v,
                                 const MbQualityThresholds *quality,
                                 unsigned *error)
{
    int source_x = (int)x + motion->dx;
    int source_y = (int)y + motion->dy;

    /* Legal table entries can still be out of bounds near frame edges. */
    if (source_x < 0 || source_y < 0 ||
        source_x + 4 > (int)reconstructed->width ||
        source_y + 4 > (int)reconstructed->height) {
        return 0;
    }
    return mb_quality_match_format19(
        source, x, y, reconstructed, (unsigned)source_x,
        (unsigned)source_y, 4U, u, v, quality, error);
}

static int find_spatial4x4(const MbFrame *source,
                           const MbFrame *reconstructed, unsigned x,
                           unsigned y, uint8_t u, uint8_t v,
                           const MbQualityThresholds *quality,
                           MbMotionVector *motion, unsigned *matched_error,
                           size_t *evaluations)
{
    unsigned best_error = UINT32_MAX;
    unsigned index;

    /* Minimise distortion; source table order breaks equal-error ties. */
    for (index = 0U; index < 8U; ++index) {
        MbMotionVector candidate;
        unsigned error;

        if (mb_motion_format19_spatial_at(
                MB_MOTION_BLOCK_4X4, index, &candidate) != REPLAY_OK) {
            return 0;
        }
        {
            int source_x = (int)x + candidate.dx;
            int source_y = (int)y + candidate.dy;
            if (source_x >= 0 && source_y >= 0 &&
                source_x + 4 <= (int)reconstructed->width &&
                source_y + 4 <= (int)reconstructed->height) {
                ++*evaluations;
            }
        }
        if (spatial_block_matches(source, reconstructed, x, y, &candidate,
                                  u, v, quality, &error) &&
            error < best_error) {
            *motion = candidate;
            best_error = error;
            if (error == 0U) {
                break;
            }
        }
    }
    if (best_error == UINT32_MAX) {
        return 0;
    }
    *matched_error = best_error;
    return 1;
}

static void copy_spatial4x4(MbFrame *reconstructed, unsigned x, unsigned y,
                            const MbMotionVector *motion)
{
    unsigned row;
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);

    /* Legal spatial vectors point backward, so source pixels are final already. */
    for (row = 0; row < 4U; ++row) {
        unsigned column;
        for (column = 0; column < 4U; ++column) {
            reconstructed->pixels[(size_t)(y + row) * reconstructed->stride +
                                  x + column] =
                reconstructed->pixels[(size_t)(source_y + row) *
                                          reconstructed->stride +
                                      source_x + column];
        }
    }
}

static CopyCandidate select_copy4x4(
    const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, unsigned x, unsigned y, uint8_t u,
    uint8_t v, int allow_stationary, int allow_temporal, int allow_spatial,
    const MbQualityThresholds *quality, unsigned loss_level,
    CodecSuperMovingBlocksPolicy policy,
    CodecSuperMovingBlocksEncodeStats *stats,
    SuperMovingBlocksWorkspaceInternal *workspace)
{
    CopyCandidate selected = {
        SPLIT_MODE_DATA, { 0, 0, 0 }, 0U, 0U, 0U, 0
    };
    CopyCandidate candidate;
    unsigned error;

    if (allow_stationary) {
        ++stats->stationary4x4_evaluations;
    }
    if (allow_stationary && block_matches_data_reconstruction(
            source, previous, x, y, u, v, quality, &error)) {
        selected = (CopyCandidate){
            SPLIT_MODE_STATIONARY, { 0, 0, 0 }, error, 2U, 0U, 1
        };
        /* Exact stationary is globally optimal under the documented policy. */
        if (policy == CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED || error == 0U) {
            return selected;
        }
    }

    candidate = (CopyCandidate){
        SPLIT_MODE_TEMPORAL, { 0, 0, 0 }, 0U, 0U, 1U, 0
    };
    if (allow_temporal && find_temporal4x4(
            source, previous, x, y, u, v, loss_level,
            &candidate.motion, &error,
            &stats->temporal4x4_evaluations, workspace)) {
        candidate.error = error;
        candidate.bits = motion_bits(&candidate.motion, MB_MOTION_BLOCK_4X4);
        candidate.valid = 1;
        if (policy == CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED) {
            return candidate;
        }
        if (candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }

    candidate = (CopyCandidate){
        SPLIT_MODE_SPATIAL, { 0, 0, 1 }, 0U, 0U, 2U, 0
    };
    if (allow_spatial && find_spatial4x4(
            source, reconstructed, x, y, u, v, quality,
            &candidate.motion, &error,
            &stats->spatial4x4_evaluations)) {
        candidate.error = error;
        candidate.bits = motion_bits(&candidate.motion, MB_MOTION_BLOCK_4X4);
        candidate.valid = 1;
        if (candidate_better(&candidate, &selected)) {
            selected = candidate;
        }
    }
    return selected;
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

static void copy_decision2x2(const MbFrame *previous, MbFrame *reconstructed,
                             unsigned x, unsigned y,
                             const SplitDecision *decision)
{
    const MbFrame *reference =
        decision->mode == SPLIT_MODE_SPATIAL ? reconstructed : previous;
    unsigned source_x = (unsigned)((int)x + decision->motion.dx);
    unsigned source_y = (unsigned)((int)y + decision->motion.dy);
    unsigned row;

    for (row = 0U; row < 2U; ++row) {
        unsigned column;
        for (column = 0U; column < 2U; ++column) {
            reconstructed->pixels[(size_t)(y + row) * reconstructed->stride +
                                  x + column] =
                reference->pixels[(size_t)(source_y + row) * reference->stride +
                                  source_x + column];
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
    ReplayBitWriter writer;
    ReplayBuffer data_candidate;
    ReplayBuffer split_candidate;
    /* Predictor lifetime is one frame and its specified initial value is zero. */
    MbPredictor predictor = { 0 };
    CodecSuperMovingBlocksEncodeStats local_stats = { 0 };
    MbQualityThresholds quality;
    int allow_stationary = options != NULL && options->allow_stationary != 0;
    int allow_temporal = options != NULL && options->allow_temporal != 0;
    int allow_spatial = options != NULL && options->allow_spatial != 0;
    int allow_split = options != NULL && options->allow_split != 0;
    CodecSuperMovingBlocksPolicy policy =
        options != NULL ? options->policy
                        : CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED;
    SuperMovingBlocksWorkspaceInternal *workspace =
        options != NULL ? workspace_internal(options->workspace) : NULL;
    unsigned loss_level = options != NULL ? options->loss_level : 0U;
    unsigned y;

    if (output == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    replay_buffer_clear(output);
    if (mb_quality_thresholds(options != NULL ? options->loss_level : 0U,
                              &quality) != REPLAY_OK) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if ((policy != CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED &&
         policy != CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR) ||
        !valid_frame_pair(source, reconstructed) ||
        !valid_source_samples(source) ||
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
    /* Reused scratch buffers avoid allocating twice for every data decision. */
    replay_buffer_init(&data_candidate);
    replay_buffer_init(&split_candidate);
    /* Raster order is part of the format, not an optimization choice. */
    for (y = 0; y < source->height; y += 4U) {
        unsigned x;
        for (x = 0; x < source->width; x += 4U) {
            uint8_t u = average_chroma4x4(source, x, y, 0);
            uint8_t v = average_chroma4x4(source, x, y, 1);
            CopyCandidate copy;
            ReplayStatus status;

            /*
             * Copy modes use the selected original-compressor quality row.
             * Priority between mode families is current encoder policy, not a
             * bitstream rule. Searches within temporal and spatial choose the
             * lowest accepted error, retaining table order on equal error.
             */
            copy = select_copy4x4(
                source, previous, reconstructed, x, y, u, v,
                allow_stationary, allow_temporal, allow_spatial,
                &quality, loss_level, policy, &local_stats, workspace);
            if (copy.mode == SPLIT_MODE_STATIONARY) {
                status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
                if (status == REPLAY_OK) {
                    copy_block4x4(previous, reconstructed, x, y);
                    ++local_stats.stationary4x4_blocks;
                }
            } else if (copy.mode == SPLIT_MODE_TEMPORAL) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(
                        &writer, MB_MOTION_BLOCK_4X4, &copy.motion);
                }
                if (status == REPLAY_OK) {
                    copy_temporal4x4(
                        previous, reconstructed, x, y, &copy.motion);
                    ++local_stats.temporal4x4_blocks;
                }
            } else if (copy.mode == SPLIT_MODE_SPATIAL) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(
                        &writer, MB_MOTION_BLOCK_4X4, &copy.motion);
                }
                if (status == REPLAY_OK) {
                    copy_spatial4x4(reconstructed, x, y, &copy.motion);
                    ++local_stats.spatial4x4_blocks;
                }
            } else {
                /* No accepted copy exists; compare literal block organizations. */
                MbPredictor data_predictor;
                MbPredictor split_predictor;
                size_t data_bits;
                size_t split_bits = SIZE_MAX;
                SplitDecision split_decisions[4];

                /*
                 * Encode both representations to scratch bitstreams. Huffman
                 * lengths and split/header overhead make formula-based guesses
                 * unnecessarily fragile. A tie stays 4x4 for simpler output.
                 */
                status = build_data4x4_candidate(
                    source, x, y, &predictor, &data_candidate,
                    &data_predictor, &data_bits);
                if (status == REPLAY_OK && allow_split) {
                    status = build_split_data_candidate(
                        source, x, y, previous, reconstructed,
                        allow_stationary, allow_temporal, allow_spatial,
                        policy, &quality, loss_level,
                        &predictor, &split_candidate, &split_predictor,
                        split_decisions, &split_bits, &local_stats, workspace);
                }
                if (status == REPLAY_OK && split_bits < data_bits) {
                    static const unsigned offsets[4][2] = {
                        { 0U, 0U }, { 2U, 0U },
                        { 0U, 2U }, { 2U, 2U }
                    };
                    unsigned block;

                    status = append_candidate(
                        &writer, &split_candidate, split_bits);
                    /* Commit both the selected bits and their resulting state. */
                    predictor = split_predictor;
                    for (block = 0U; status == REPLAY_OK && block < 4U;
                         ++block) {
                        unsigned block_x = x + offsets[block][0];
                        unsigned block_y = y + offsets[block][1];
                        if (split_decisions[block].mode ==
                            SPLIT_MODE_STATIONARY) {
                            copy_decision2x2(
                                previous, reconstructed, block_x, block_y,
                                &split_decisions[block]);
                            ++local_stats.stationary2x2_blocks;
                        } else if (split_decisions[block].mode ==
                                   SPLIT_MODE_TEMPORAL) {
                            copy_decision2x2(
                                previous, reconstructed, block_x, block_y,
                                &split_decisions[block]);
                            ++local_stats.temporal2x2_blocks;
                        } else if (split_decisions[block].mode ==
                                   SPLIT_MODE_SPATIAL) {
                            copy_decision2x2(
                                previous, reconstructed, block_x, block_y,
                                &split_decisions[block]);
                            ++local_stats.spatial2x2_blocks;
                        } else {
                            uint8_t block_u = average_chroma2x2(
                                source, block_x, block_y, 0);
                            uint8_t block_v = average_chroma2x2(
                                source, block_x, block_y, 1);
                            reconstruct_data2x2(
                                source, reconstructed, block_x, block_y,
                                block_u, block_v);
                            ++local_stats.data2x2_blocks;
                        }
                    }
                    if (status == REPLAY_OK) {
                        ++local_stats.split4x4_blocks;
                    }
                } else if (status == REPLAY_OK) {
                    status = append_candidate(
                        &writer, &data_candidate, data_bits);
                    predictor = data_predictor;
                    if (status == REPLAY_OK) {
                        reconstruct_data4x4(
                            source, reconstructed, x, y, u, v);
                        ++local_stats.data4x4_blocks;
                    }
                }
            }
            if (status != REPLAY_OK) {
                replay_buffer_free(&split_candidate);
                replay_buffer_free(&data_candidate);
                replay_buffer_clear(output);
                return status;
            }
        }
    }
    /* Report semantic bits; the final payload may contain zero pad bits. */
    local_stats.bits_written = replay_bitwriter_position(&writer);
    {
        ReplayStatus status = replay_bitwriter_flush_zero(&writer);
        if (status != REPLAY_OK) {
            replay_buffer_free(&split_candidate);
            replay_buffer_free(&data_candidate);
            replay_buffer_clear(output);
            return status;
        }
    }
    replay_buffer_free(&split_candidate);
    replay_buffer_free(&data_candidate);
    if (stats != NULL) {
        *stats = local_stats;
    }
    return REPLAY_OK;
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
                                unsigned x, unsigned y, MbVerifyError *error,
                                MbMotionVector *decoded_motion)
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
    /* Spatial copies read completed current-frame pixels; temporal copies read
       the immutable previous reconstruction. */
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

    if (motion.spatial) {
        unsigned parent_x = x & ~3U;
        unsigned parent_y = y & ~3U;
        unsigned current_quadrant =
            block_size == MB_MOTION_BLOCK_2X2
                ? ((y - parent_y) / 2U) * 2U + (x - parent_x) / 2U
                : 0U;

        for (row = 0U; row < (unsigned)block_size; ++row) {
            unsigned column;

            for (column = 0U; column < (unsigned)block_size; ++column) {
                unsigned pixel_x = (unsigned)source_x + column;
                unsigned pixel_y = (unsigned)source_y + row;
                unsigned source_block_x = pixel_x & ~3U;
                unsigned source_block_y = pixel_y & ~3U;
                int available =
                    source_block_y < parent_y ||
                    (source_block_y == parent_y &&
                     source_block_x < parent_x);

                if (!available && block_size == MB_MOTION_BLOCK_2X2 &&
                    source_block_x == parent_x &&
                    source_block_y == parent_y) {
                    unsigned source_quadrant =
                        ((pixel_y - parent_y) / 2U) * 2U +
                        (pixel_x - parent_x) / 2U;
                    available = source_quadrant < current_quadrant;
                }
                if (!available) {
                    set_verify_error(
                        error, reader, x, y,
                        "spatial reference has not been reconstructed");
                    return REPLAY_MALFORMED_STREAM;
                }
            }
        }
    }

    for (row = 0; row < (unsigned)block_size; ++row) {
        unsigned column;
        for (column = 0; column < (unsigned)block_size; ++column) {
            decoded->pixels[(y + row) * decoded->stride + x + column] =
                source->pixels[((unsigned)source_y + row) * source->stride +
                               (unsigned)source_x + column];
        }
    }
    if (decoded_motion != NULL) {
        *decoded_motion = motion;
    }
    return REPLAY_OK;
}

static void trace_decoded_block(CodecSuperMovingBlocksDecodeTrace trace,
                                void *opaque, unsigned x, unsigned y,
                                unsigned size,
                                CodecSuperMovingBlocksMode mode,
                                size_t bit_start, size_t bit_end,
                                const MbMotionVector *motion)
{
    CodecSuperMovingBlocksDecodeEvent event;

    if (trace == NULL) {
        return;
    }
    event.x = x;
    event.y = y;
    event.size = size;
    event.mode = mode;
    event.bit_start = bit_start;
    event.bit_end = bit_end;
    event.motion_dx = motion != NULL ? motion->dx : 0;
    event.motion_dy = motion != NULL ? motion->dy : 0;
    trace(&event, opaque);
}

static ReplayStatus verify_2x2(ReplayBitReader *reader,
                               MbPredictor *predictor,
                               const MbFrame *previous, MbFrame *decoded,
                               unsigned x, unsigned y, MbVerifyError *error,
                               CodecSuperMovingBlocksDecodeTrace trace,
                               void *trace_opaque)
{
    /*
     * 2x2 opcodes are prefix-shaped rather than a uniform two-bit enum:
     * `1` is motion, `00` is stationary, and `01` begins a 12-bit data header.
     * Save the reader so the data decoder can consume that header from bit 0.
     */
    ReplayBitReader start = *reader;
    size_t bit_start = replay_bitreader_position(reader);
    uint32_t first;
    ReplayStatus status = replay_bitreader_read(reader, 1U, &first);

    if (status != REPLAY_OK) {
        set_verify_error(error, reader, x, y, "truncated 2x2 opcode");
        return status;
    }
    if (first != 0U) {
        MbMotionVector motion;
        status = copy_motion(reader, MB_MOTION_BLOCK_2X2, previous, decoded,
                             x, y, error, &motion);
        if (status == REPLAY_OK) {
            trace_decoded_block(
                trace, trace_opaque, x, y, 2U,
                motion.spatial != 0 ? CODEC_SUPERMOVINGBLOCKS_MODE_SPATIAL
                                    : CODEC_SUPERMOVINGBLOCKS_MODE_TEMPORAL,
                bit_start, replay_bitreader_position(reader), &motion);
        }
        return status;
    }

    {
        uint32_t second;
        status = replay_bitreader_read(reader, 1U, &second);
        if (status != REPLAY_OK) {
            set_verify_error(error, reader, x, y, "truncated 2x2 opcode");
            return status;
        }
        if (second == 0U) {
            status = copy_stationary(
                previous, decoded, x, y, 2U, reader, error);
            if (status == REPLAY_OK) {
                trace_decoded_block(
                    trace, trace_opaque, x, y, 2U,
                    CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY, bit_start,
                    replay_bitreader_position(reader), NULL);
            }
            return status;
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
    if (status == REPLAY_OK) {
        trace_decoded_block(trace, trace_opaque, x, y, 2U,
                            CODEC_SUPERMOVINGBLOCKS_MODE_DATA, bit_start,
                            replay_bitreader_position(reader), NULL);
    }
    return status;
}

ReplayStatus codec_supermovingblocks_verify_frame_traced(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    CodecSuperMovingBlocksDecodeTrace trace, void *trace_opaque)
{
    ReplayBitReader reader;
    /* This must evolve exactly as in the original decompressor, independently
       of which choices the portable encoder currently knows how to make. */
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

    /* The scan order is normative because spatial copies and the predictor
       both depend on all earlier decisions having been reconstructed. */
    for (y = 0; y < decoded->height; y += 4U) {
        unsigned x;
        for (x = 0; x < decoded->width; x += 4U) {
            ReplayBitReader start = reader;
            size_t bit_start = replay_bitreader_position(&reader);
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
                if (status == REPLAY_OK) {
                    trace_decoded_block(
                        trace, trace_opaque, x, y, 4U,
                        CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY, bit_start,
                        replay_bitreader_position(&reader), NULL);
                }
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
                if (status == REPLAY_OK) {
                    trace_decoded_block(
                        trace, trace_opaque, x, y, 4U,
                        CODEC_SUPERMOVINGBLOCKS_MODE_DATA, bit_start,
                        replay_bitreader_position(&reader), NULL);
                }
                break;
            case 2U: {
                MbMotionVector motion;
                status = copy_motion(&reader, MB_MOTION_BLOCK_4X4,
                                     previous, decoded, x, y, error, &motion);
                if (status == REPLAY_OK) {
                    trace_decoded_block(
                        trace, trace_opaque, x, y, 4U,
                        motion.spatial != 0
                            ? CODEC_SUPERMOVINGBLOCKS_MODE_SPATIAL
                            : CODEC_SUPERMOVINGBLOCKS_MODE_TEMPORAL,
                        bit_start, replay_bitreader_position(&reader),
                        &motion);
                }
                break;
            }
            case 3U:
                /* Split sub-block order is top-left, top-right, bottom-left,
                   bottom-right, matching the original decompressor. */
                status = verify_2x2(&reader, &predictor, previous, decoded,
                                    x, y, error, trace, trace_opaque);
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x + 2U, y, error, trace,
                                        trace_opaque);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x, y + 2U, error, trace,
                                        trace_opaque);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(&reader, &predictor, previous, decoded,
                                        x + 2U, y + 2U, error, trace,
                                        trace_opaque);
                }
                if (status == REPLAY_OK) {
                    trace_decoded_block(
                        trace, trace_opaque, x, y, 4U,
                        CODEC_SUPERMOVINGBLOCKS_MODE_SPLIT, bit_start,
                        replay_bitreader_position(&reader), NULL);
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

        /* Strict framing catches wrong dimensions and accidental concatenation. */
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
