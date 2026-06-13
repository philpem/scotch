#include "replay/codec_movingblockshq.h"

#include "replay/mb_encode.h"
#include "replay/mb_frame_verify.h"
#include "replay/mb_motion.h"
#include "replay/mb_quality.h"
#include "replay/replay_bitstream.h"
#include "replay/replay_buffer.h"

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

/*
 * Type-17 instance of the shared mb_encode search hook table. The motion tables
 * are common to the Moving Blocks family, so the vector accessors reuse the
 * format-19 enumerators; only the block-match metric is YUV555-specific.
 */
static int codec17_temporal_vector(unsigned index, MbMotionVector *out)
{
    return mb_motion_format19_temporal_at(index, out) == REPLAY_OK;
}

static int codec17_spatial_vector(MbMotionBlockSize block_size, unsigned index,
                                  MbMotionVector *out)
{
    return mb_motion_format19_spatial_at(block_size, index, out) == REPLAY_OK;
}

static int codec17_block_match(const MbFrame *source, unsigned x, unsigned y,
                               const MbFrame *reference, unsigned ref_x,
                               unsigned ref_y, unsigned size, uint8_t u,
                               uint8_t v, const void *quality, unsigned *error)
{
    return mb_quality_match_format17(source, x, y, reference, ref_x, ref_y,
                                     size, u, v,
                                     (const MbQualityThresholds *)quality,
                                     error);
}

static int codec17_profile_match(const MbFrame *source, unsigned x, unsigned y,
                                 const MbFrame *reference, unsigned ref_x,
                                 unsigned ref_y, unsigned size, uint8_t u,
                                 uint8_t v, unsigned *total_error,
                                 unsigned *first_level)
{
    MbQualityProfile profile;

    if (!mb_quality_profile_format17(source, x, y, reference, ref_x, ref_y,
                                     size, u, v, &profile)) {
        return 0;
    }
    *total_error = profile.total_error;
    *first_level = mb_quality_first_accepted_level(&profile, size);
    return 1;
}

static const MbEncodeCodec codec17_encode = {
    MB_QUALITY_LEVEL_COUNT,
    288U,
    8U,
    codec17_temporal_vector,
    codec17_spatial_vector,
    codec17_block_match,
    codec17_profile_match
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

static ReplayStatus write_header(ReplayBitWriter *writer, uint32_t opcode,
                                 uint8_t u, uint8_t v)
{
    uint32_t header;

    if (writer == NULL || u > 31U || v > 31U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /* opcode:2, U:5, V:5 in stream-bit order, matching read_header. */
    header = opcode | ((uint32_t)u << 2U) | ((uint32_t)v << 7U);
    return replay_bitwriter_write(writer, header, 12U);
}

/* Average chroma of a `size`x`size` source block, kept as a 5-bit value. */
static uint8_t avg5(const MbPixel *pixels, size_t stride, unsigned size,
                    int chroma_v)
{
    unsigned sum = 0U;
    unsigned n = size * size;
    unsigned row;

    for (row = 0U; row < size; ++row) {
        unsigned col;

        for (col = 0U; col < size; ++col) {
            const MbPixel *p = &pixels[(size_t)row * stride + col];

            sum += chroma_v ? p->v : p->u;
        }
    }
    return (uint8_t)(((sum + n / 2U) / n) & 31U);
}

/*
 * Core 4x4 data encoder allowing independent source and reconstruction strides.
 * The split path reconstructs into a compact tentative buffer while reading the
 * full-stride source; the public wrapper passes one stride for both.
 */
static ReplayStatus encode_data4x4_core(
    ReplayBitWriter *writer, const MbPixel *source, size_t source_stride,
    MbPixel *recon, size_t recon_stride, MbPredictor *predictor)
{
    uint8_t u;
    uint8_t v;
    unsigned sum = 0U;
    unsigned row;
    ReplayStatus status;

    if (writer == NULL || source == NULL || recon == NULL ||
        predictor == NULL || source_stride < 4U || recon_stride < 4U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    u = avg5(source, source_stride, 4U, 0);
    v = avg5(source, source_stride, 4U, 1);
    status = write_header(writer, UINT32_C(1), u, v);
    for (row = 0U; status == REPLAY_OK && row < 4U; ++row) {
        unsigned column;

        for (column = 0U; status == REPLAY_OK && column < 4U; ++column) {
            MbPixel *out = &recon[(size_t)row * recon_stride + column];
            unsigned target = source[(size_t)row * source_stride + column].y;
            unsigned prediction;
            uint8_t residual;

            if (row == 0U) {
                prediction = column == 0U ? predictor->luma
                                          : recon[column - 1U].y;
            } else if (column == 0U) {
                prediction = recon[(size_t)(row - 1U) * recon_stride].y;
            } else {
                prediction =
                    ((unsigned)recon[(size_t)row * recon_stride + column - 1U].y +
                     (unsigned)recon[(size_t)(row - 1U) * recon_stride + column].y)
                    >> 1U;
            }
            residual = (uint8_t)((target - prediction) & 31U);
            status = mb_huffman_write(writer,
                                      &codec_movingblockshq_luma_huffman,
                                      residual);
            out->y = (uint8_t)((prediction + residual) & 31U);
            out->u = u;
            out->v = v;
            sum += out->y;
        }
    }
    if (status == REPLAY_OK) {
        predictor->luma = (uint8_t)(sum >> 4U);
    }
    return status;
}

/* Core 2x2 data encoder with independent source and reconstruction strides. */
static ReplayStatus encode_data2x2_core(
    ReplayBitWriter *writer, const MbPixel *source, size_t source_stride,
    MbPixel *recon, size_t recon_stride, MbPredictor *predictor)
{
    uint8_t u;
    uint8_t v;
    unsigned sum = 0U;
    unsigned index;
    ReplayStatus status;

    if (writer == NULL || source == NULL || recon == NULL ||
        predictor == NULL || source_stride < 2U || recon_stride < 2U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    u = avg5(source, source_stride, 2U, 0);
    v = avg5(source, source_stride, 2U, 1);
    status = write_header(writer, UINT32_C(2), u, v);
    for (index = 0U; status == REPLAY_OK && index < 4U; ++index) {
        unsigned row = index >> 1U;
        unsigned column = index & 1U;
        MbPixel *out = &recon[(size_t)row * recon_stride + column];
        unsigned target = source[(size_t)row * source_stride + column].y;
        unsigned prediction;
        uint8_t residual;

        if (index == 0U) {
            prediction = predictor->luma;
        } else if (index == 1U || index == 2U) {
            prediction = recon[0].y;
        } else {
            prediction = ((unsigned)recon[1].y +
                          (unsigned)recon[recon_stride].y) >> 1U;
        }
        residual = (uint8_t)((target - prediction) & 31U);
        status = mb_huffman_write(writer, &codec_movingblockshq_luma_huffman,
                                  residual);
        out->y = (uint8_t)((prediction + residual) & 31U);
        out->u = u;
        out->v = v;
        sum += out->y;
    }
    if (status == REPLAY_OK) {
        predictor->luma = (uint8_t)(sum >> 2U);
    }
    return status;
}

/*
 * Encode one 4x4 source block as a data-coded 4x4. Luma is lossless: the normal
 * 32-symbol table codes every residual, so each reconstructed pixel equals the
 * source. Predictions follow already-reconstructed neighbours exactly as the
 * decoder does, and the carried predictor becomes the block's truncated mean.
 */
ReplayStatus codec_movingblockshq_encode_data4x4(
    ReplayBitWriter *writer, const MbPixel *source, MbPixel *recon,
    size_t stride, MbPredictor *predictor)
{
    return encode_data4x4_core(writer, source, stride, recon, stride,
                               predictor);
}

/* Encode one 2x2 source block as a data-coded 2x2 (split child). */
ReplayStatus codec_movingblockshq_encode_data2x2(
    ReplayBitWriter *writer, const MbPixel *source, MbPixel *recon,
    size_t stride, MbPredictor *predictor)
{
    return encode_data2x2_core(writer, source, stride, recon, stride,
                               predictor);
}

/* A frame the encoder can scan: 4x4-aligned, non-empty, stride covers width. */
static int frame_dims_ok(const MbFrame *frame)
{
    return frame != NULL && frame->pixels != NULL && frame->width != 0U &&
           frame->height != 0U && (frame->width & 3U) == 0U &&
           (frame->height & 3U) == 0U && frame->stride >= frame->width;
}

ReplayStatus codec_movingblockshq_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written)
{
    ReplayBitWriter writer;
    /* Predictor lifetime is one frame; the format clears it to zero on entry. */
    MbPredictor predictor = { 0 };
    unsigned y;
    ReplayStatus status = REPLAY_OK;

    if (output == NULL || !frame_dims_ok(source) ||
        !frame_dims_ok(reconstructed) ||
        reconstructed->width != source->width ||
        reconstructed->height != source->height ||
        reconstructed->stride != source->stride) {
        return REPLAY_INVALID_ARGUMENT;
    }
    replay_buffer_clear(output);
    replay_bitwriter_init(&writer, output);
    /* Raster scan of 4x4 blocks is normative for both prediction and output. */
    for (y = 0U; status == REPLAY_OK && y < source->height; y += 4U) {
        unsigned x;

        for (x = 0U; status == REPLAY_OK && x < source->width; x += 4U) {
            size_t offset = (size_t)y * source->stride + x;

            status = codec_movingblockshq_encode_data4x4(
                &writer, &source->pixels[offset],
                &reconstructed->pixels[offset], source->stride, &predictor);
        }
    }
    if (status == REPLAY_OK && bits_written != NULL) {
        *bits_written = replay_bitwriter_position(&writer);
    }
    if (status == REPLAY_OK) {
        /* Decomp17 reads whole bytes; pad the final partial byte with zeroes. */
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Append `bits` meaningful bits of a byte-padded candidate buffer to the live
 * stream, dropping the candidate's own zero padding. */
static ReplayStatus append_candidate(ReplayBitWriter *writer,
                                     const ReplayBuffer *buffer, size_t bits)
{
    ReplayBitReader reader;

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

/*
 * Record a 2x2 copy child's decoded pixels in the parent's tentative 4x4
 * reconstruction so a later spatial child can reference them. Spatial sources
 * inside the parent read earlier tentative pixels; temporal/stationary sources
 * and out-of-parent spatial sources read finished frames.
 */
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
                pixel = mb_encode_split_spatial_pixel(
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

/*
 * Choose the best 2x2 child mode under the ordered policy: stationary, then
 * temporal, then spatial copy, else a data block. `u`/`v` are the data-block
 * average chroma the copy candidates are scored against.
 */
static CopyCandidate select_copy2x2_hq(
    const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, const MbPixel tentative[16],
    unsigned available_mask, unsigned parent_x, unsigned parent_y,
    unsigned x, unsigned y, uint8_t u, uint8_t v, int allow_stationary,
    int allow_temporal, int allow_spatial,
    const MbQualityThresholds *quality, unsigned loss_level)
{
    CopyCandidate candidate = { SPLIT_MODE_DATA, { 0, 0, 0 }, 0U, 0U, 0U, 0 };
    unsigned error;
    size_t evaluations = 0U;

    if (allow_stationary && previous != NULL &&
        mb_quality_match_format17(source, x, y, previous, x, y, 2U, u, v,
                                  quality, &error)) {
        candidate.mode = SPLIT_MODE_STATIONARY;
        return candidate;
    }
    if (allow_temporal && previous != NULL &&
        mb_encode_match_temporal2x2(&codec17_encode, source, previous, x, y,
                                    u, v, loss_level, &candidate.motion,
                                    &error, &evaluations, NULL)) {
        candidate.mode = SPLIT_MODE_TEMPORAL;
        return candidate;
    }
    if (allow_spatial &&
        mb_encode_match_spatial2x2(&codec17_encode, source, reconstructed,
                                   tentative, available_mask, parent_x,
                                   parent_y, x, y, u, v, quality,
                                   &candidate.motion, &error, &evaluations)) {
        candidate.mode = SPLIT_MODE_SPATIAL;
        return candidate;
    }
    candidate.mode = SPLIT_MODE_DATA;
    return candidate;
}

/* Encode a data-coded 4x4 into a scratch buffer, reconstructing into a compact
 * 16-pixel block; reports the meaningful bit count. */
static ReplayStatus build_data4x4_candidate(
    const MbFrame *source, unsigned x, unsigned y,
    const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPixel recon[16], MbPredictor *final_predictor, size_t *bits)
{
    ReplayBitWriter writer;
    ReplayStatus status;

    replay_buffer_clear(buffer);
    replay_bitwriter_init(&writer, buffer);
    *final_predictor = *initial_predictor;
    status = encode_data4x4_core(
        &writer, &source->pixels[(size_t)y * source->stride + x],
        source->stride, recon, 4U, final_predictor);
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/*
 * Encode the four 2x2 children of a split block into a scratch buffer, choosing
 * each child's mode and filling `tentative` with the parent's reconstruction.
 * `modes` reports each child's mode for block accounting; `bits` is the
 * meaningful split bit count (including the `11` split prefix).
 */
static ReplayStatus build_split_candidate(
    const MbFrame *source, const MbFrame *previous,
    const MbFrame *reconstructed, unsigned x, unsigned y,
    int allow_stationary, int allow_temporal, int allow_spatial,
    const MbQualityThresholds *quality, unsigned loss_level,
    const MbPredictor *initial_predictor, ReplayBuffer *buffer,
    MbPredictor *final_predictor, MbPixel tentative[16], SplitMode modes[4],
    size_t *bits)
{
    /* Decoder order inside a split block is TL, TR, BL, BR. */
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
    *final_predictor = *initial_predictor;
    status = replay_bitwriter_write(&writer, UINT32_C(3), 2U);
    for (block = 0U; status == REPLAY_OK && block < 4U; ++block) {
        unsigned block_x = x + offsets[block][0];
        unsigned block_y = y + offsets[block][1];
        unsigned local = (block_y - y) * 4U + block_x - x;
        size_t child = (size_t)block_y * source->stride + block_x;
        uint8_t u = avg5(&source->pixels[child], source->stride, 2U, 0);
        uint8_t v = avg5(&source->pixels[child], source->stride, 2U, 1);
        CopyCandidate selected = select_copy2x2_hq(
            source, previous, reconstructed, tentative, available_mask,
            x, y, block_x, block_y, u, v, allow_stationary, allow_temporal,
            allow_spatial, quality, loss_level);

        modes[block] = selected.mode;
        if (selected.mode == SPLIT_MODE_STATIONARY) {
            status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
            fill_tentative_copy2x2(previous, reconstructed, tentative, x, y,
                                   block_x, block_y, &selected.motion,
                                   &available_mask);
        } else if (selected.mode == SPLIT_MODE_TEMPORAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(
                    &writer, MB_MOTION_BLOCK_2X2, &selected.motion);
            }
            fill_tentative_copy2x2(previous, reconstructed, tentative, x, y,
                                   block_x, block_y, &selected.motion,
                                   &available_mask);
        } else if (selected.mode == SPLIT_MODE_SPATIAL) {
            status = replay_bitwriter_write(&writer, UINT32_C(1), 1U);
            if (status == REPLAY_OK) {
                status = mb_motion_write_format19(
                    &writer, MB_MOTION_BLOCK_2X2, &selected.motion);
            }
            fill_tentative_copy2x2(reconstructed, reconstructed, tentative,
                                   x, y, block_x, block_y, &selected.motion,
                                   &available_mask);
        } else {
            /* The core writes the block, advances the predictor, and fills the
               child's four tentative pixels (luma lossless, chroma averaged). */
            status = encode_data2x2_core(&writer, &source->pixels[child],
                                         source->stride, &tentative[local], 4U,
                                         final_predictor);
            available_mask |= 1U << local;
            available_mask |= 1U << (local + 1U);
            available_mask |= 1U << (local + 4U);
            available_mask |= 1U << (local + 5U);
        }
    }
    *bits = replay_bitwriter_position(&writer);
    if (status == REPLAY_OK) {
        status = replay_bitwriter_flush_zero(&writer);
    }
    return status;
}

/* Write a reconstructed 4x4 block (compact 16-pixel layout) into the frame. */
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

/* Copy a 4x4 block from `reference` at the motion offset to `destination`. The
 * reference is the previous frame for temporal copies or the reconstruction in
 * progress for spatial copies, whose vectors always point at finished pixels. */
static void copy_motion4x4(const MbFrame *reference, MbFrame *destination,
                           unsigned x, unsigned y, const MbMotionVector *motion)
{
    unsigned source_x = (unsigned)((int)x + motion->dx);
    unsigned source_y = (unsigned)((int)y + motion->dy);
    unsigned row;

    for (row = 0U; row < 4U; ++row) {
        unsigned column;
        for (column = 0U; column < 4U; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] =
                reference->pixels[(size_t)(source_y + row) * reference->stride +
                                  source_x + column];
        }
    }
}

/* Copy a 4x4 block straight across (the stationary previous-frame copy). */
static void copy_stationary4x4(const MbFrame *previous, MbFrame *destination,
                               unsigned x, unsigned y)
{
    unsigned row;

    for (row = 0U; row < 4U; ++row) {
        unsigned column;
        for (column = 0U; column < 4U; ++column) {
            destination->pixels[(size_t)(y + row) * destination->stride +
                                x + column] =
                previous->pixels[(size_t)(y + row) * previous->stride +
                                 x + column];
        }
    }
}

ReplayStatus codec_movingblockshq_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksHqEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksHqEncodeStats *stats)
{
    ReplayBitWriter writer;
    /* The luma predictor lives for one frame and starts at zero; only data
       blocks advance it, so copy decisions leave it untouched. */
    MbPredictor predictor = { 0 };
    MbQualityThresholds thresholds;
    CodecMovingBlocksHqEncodeStats local_stats = { 0 };
    int allow_stationary = options != NULL && options->allow_stationary != 0;
    int allow_temporal = options != NULL && options->allow_temporal != 0;
    int allow_spatial = options != NULL && options->allow_spatial != 0;
    int allow_split = options != NULL && options->allow_split != 0;
    unsigned loss_level = options != NULL ? options->loss_level : 0U;
    int need_previous = allow_stationary || allow_temporal;
    ReplayBuffer data_candidate;
    ReplayBuffer split_candidate;
    unsigned y;
    ReplayStatus status = REPLAY_OK;

    if (output != NULL) {
        replay_buffer_clear(output);
    }
    if (output == NULL || !frame_dims_ok(source) ||
        !frame_dims_ok(reconstructed) ||
        reconstructed->width != source->width ||
        reconstructed->height != source->height ||
        reconstructed->stride != source->stride ||
        mb_quality_thresholds(loss_level, &thresholds) != REPLAY_OK ||
        (need_previous &&
         (previous == NULL || previous->pixels == NULL ||
          previous->width != source->width ||
          previous->height != source->height ||
          previous->stride < previous->width))) {
        return REPLAY_INVALID_ARGUMENT;
    }

    replay_bitwriter_init(&writer, output);
    /* Reused scratch buffers avoid reallocating for every data/split decision. */
    replay_buffer_init(&data_candidate);
    replay_buffer_init(&split_candidate);
    for (y = 0U; status == REPLAY_OK && y < source->height; y += 4U) {
        unsigned x;

        for (x = 0U; status == REPLAY_OK && x < source->width; x += 4U) {
            size_t offset = (size_t)y * source->stride + x;
            uint8_t u = avg5(&source->pixels[offset], source->stride, 4U, 0);
            uint8_t v = avg5(&source->pixels[offset], source->stride, 4U, 1);
            MbMotionVector motion;
            unsigned error;
            size_t evaluations = 0U;

            /* Ordered copy policy: stationary, then temporal, then spatial. */
            if (allow_stationary &&
                mb_quality_match_format17(source, x, y, previous, x, y, 4U,
                                          u, v, &thresholds, &error)) {
                status = replay_bitwriter_write(&writer, UINT32_C(0), 2U);
                copy_stationary4x4(previous, reconstructed, x, y);
                ++local_stats.stationary4x4_blocks;
            } else if (allow_temporal &&
                       mb_encode_find_temporal4x4(
                           &codec17_encode, source, previous, x, y, u, v,
                           loss_level, &motion, &error, &evaluations, NULL)) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(
                        &writer, MB_MOTION_BLOCK_4X4, &motion);
                }
                copy_motion4x4(previous, reconstructed, x, y, &motion);
                ++local_stats.temporal4x4_blocks;
            } else if (allow_spatial &&
                       mb_encode_find_spatial4x4(
                           &codec17_encode, source, reconstructed, x, y, u, v,
                           &thresholds, &motion, &error, &evaluations)) {
                status = replay_bitwriter_write(&writer, UINT32_C(2), 2U);
                if (status == REPLAY_OK) {
                    status = mb_motion_write_format19(
                        &writer, MB_MOTION_BLOCK_4X4, &motion);
                }
                copy_motion4x4(reconstructed, reconstructed, x, y, &motion);
                ++local_stats.spatial4x4_blocks;
            } else {
                /* No accepted copy: compare a data 4x4 against a 2x2 split and
                   keep whichever encodes in fewer bits (ties stay 4x4). */
                MbPixel data_recon[16];
                MbPixel split_recon[16];
                MbPredictor data_predictor;
                MbPredictor split_predictor;
                SplitMode split_modes[4];
                size_t data_bits;
                size_t split_bits = SIZE_MAX;

                status = build_data4x4_candidate(
                    source, x, y, &predictor, &data_candidate, data_recon,
                    &data_predictor, &data_bits);
                if (status == REPLAY_OK && allow_split) {
                    status = build_split_candidate(
                        source, previous, reconstructed, x, y,
                        allow_stationary, allow_temporal, allow_spatial,
                        &thresholds, loss_level, &predictor, &split_candidate,
                        &split_predictor, split_recon, split_modes,
                        &split_bits);
                }
                if (status == REPLAY_OK && split_bits < data_bits) {
                    unsigned block;

                    status = append_candidate(&writer, &split_candidate,
                                              split_bits);
                    predictor = split_predictor;
                    blit_block4x4(reconstructed, x, y, split_recon);
                    for (block = 0U; block < 4U; ++block) {
                        switch (split_modes[block]) {
                        case SPLIT_MODE_STATIONARY:
                            ++local_stats.stationary2x2_blocks;
                            break;
                        case SPLIT_MODE_TEMPORAL:
                            ++local_stats.temporal2x2_blocks;
                            break;
                        case SPLIT_MODE_SPATIAL:
                            ++local_stats.spatial2x2_blocks;
                            break;
                        default:
                            ++local_stats.data2x2_blocks;
                            break;
                        }
                    }
                    ++local_stats.split4x4_blocks;
                } else if (status == REPLAY_OK) {
                    status = append_candidate(&writer, &data_candidate,
                                              data_bits);
                    predictor = data_predictor;
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
    if (status == REPLAY_OK && stats != NULL) {
        *stats = local_stats;
    }
    return status;
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
