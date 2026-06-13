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
 * format-19 enumerators. Type 17 also shares the 6Y5UV signed chroma model and
 * measures luma the same way (the 5-bit vs 6-bit Y depth is immaterial to the
 * absolute error metric), so the quality hooks use the format-19 functions.
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
    return mb_quality_match_format19(source, x, y, reference, ref_x, ref_y,
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

    if (!mb_quality_profile_format19(source, x, y, reference, ref_x, ref_y,
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
    codec17_profile_match,
    mb_encode_motion_bits
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

static int signed_chroma(uint8_t value)
{
    /* Five-bit chroma is stored as two's-complement modulo 32, as in 6Y5UV. */
    return value < 16U ? (int)value : (int)value - 32;
}

/*
 * Signed average chroma of a `size`x`size` block as a 5-bit value. Type 17
 * shares the 6Y5UV signed chroma model, so this floors like the source ASR and
 * matches the format-19 quality metric used to score copies.
 */
static uint8_t avg5(const MbPixel *pixels, size_t stride, unsigned size,
                    int chroma_v)
{
    int sum = 0;
    int area = (int)(size * size);
    unsigned row;

    for (row = 0U; row < size; ++row) {
        unsigned col;

        for (col = 0U; col < size; ++col) {
            const MbPixel *p = &pixels[(size_t)row * stride + col];

            sum += signed_chroma(chroma_v ? p->v : p->u);
        }
    }
    if (sum < 0) {
        sum = -((-sum + area - 1) / area);
    } else {
        sum /= area;
    }
    return (uint8_t)(sum & 31);
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

/*
 * Data-block coding for the shared frame encoder: dispatch to the two-stride
 * data cores, which write the block, reconstruct it (luma lossless, chroma the
 * 5-bit signed average) and advance the 2D luma predictor.
 */
static ReplayStatus codec17_encode_data(ReplayBitWriter *writer,
                                        const MbFrame *source, unsigned x,
                                        unsigned y, unsigned size,
                                        MbPixel *recon, size_t recon_stride,
                                        MbPredictor *predictor)
{
    const MbPixel *src = &source->pixels[(size_t)y * source->stride + x];

    if (size == 4U) {
        return encode_data4x4_core(writer, src, source->stride, recon,
                                   recon_stride, predictor);
    }
    return encode_data2x2_core(writer, src, source->stride, recon,
                               recon_stride, predictor);
}

static const MbEncodeDataCodec codec17_data = { codec17_encode_data };

ReplayStatus codec_movingblockshq_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecMovingBlocksHqEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecMovingBlocksHqEncodeStats *stats)
{
    MbEncodeOptions encode_options;
    MbEncodeStats encode_stats;
    ReplayStatus status;

    /*
     * Thin adapter over the shared frame encoder. Type 17's only specifics are
     * its data-block coder (codec17_data) and YUV555 quality model (reached via
     * codec17_encode); it always uses the ordered copy policy. Stats are copied
     * into the public block-count struct (type 17 keeps no evaluation counters).
     */
    encode_options.allow_stationary =
        options != NULL && options->allow_stationary != 0;
    encode_options.allow_temporal =
        options != NULL && options->allow_temporal != 0;
    encode_options.allow_spatial =
        options != NULL && options->allow_spatial != 0;
    encode_options.allow_split = options != NULL && options->allow_split != 0;
    encode_options.loss_level = options != NULL ? options->loss_level : 0U;
    encode_options.policy =
        options != NULL ? options->policy : MB_ENCODE_POLICY_ORDERED;
    encode_options.workspace = options != NULL ? options->workspace : NULL;

    status = mb_encode_frame(&codec17_encode, &codec17_data, source, previous,
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
        stats->bits_written = encode_stats.bits_written;
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
