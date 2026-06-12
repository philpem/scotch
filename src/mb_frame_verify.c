#include "replay/mb_frame_verify.h"

#include "replay/mb_motion.h"

static void set_error(MbVerifyError *error, const ReplayBitReader *reader,
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
        set_error(error, reader, x, y,
                  "stationary block requires a previous frame");
        return REPLAY_MALFORMED_STREAM;
    }
    if (previous->width != decoded->width ||
        previous->height != decoded->height ||
        previous->stride < decoded->width) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (row = 0U; row < size; ++row) {
        unsigned column;

        for (column = 0U; column < size; ++column) {
            decoded->pixels[(y + row) * decoded->stride + x + column] =
                previous->pixels[(y + row) * previous->stride + x + column];
        }
    }
    return REPLAY_OK;
}

static int spatial_pixel_available(unsigned destination_x,
                                   unsigned destination_y,
                                   MbMotionBlockSize block_size,
                                   unsigned source_x, unsigned source_y)
{
    unsigned parent_x = destination_x & ~3U;
    unsigned parent_y = destination_y & ~3U;
    unsigned source_block_x = source_x & ~3U;
    unsigned source_block_y = source_y & ~3U;
    int available = source_block_y < parent_y ||
                    (source_block_y == parent_y &&
                     source_block_x < parent_x);

    /*
     * A split child may also refer to an earlier child in the same 4x4
     * parent. Quadrant order is TL, TR, BL, BR, matching decoder scan order.
     */
    if (!available && block_size == MB_MOTION_BLOCK_2X2 &&
        source_block_x == parent_x && source_block_y == parent_y) {
        unsigned current_quadrant =
            ((destination_y - parent_y) / 2U) * 2U +
            (destination_x - parent_x) / 2U;
        unsigned source_quadrant =
            ((source_y - parent_y) / 2U) * 2U +
            (source_x - parent_x) / 2U;

        available = source_quadrant < current_quadrant;
    }
    return available;
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
    ReplayStatus status = mb_motion_read_hq(reader, block_size, &motion);

    if (status != REPLAY_OK) {
        set_error(error, reader, x, y, "invalid or truncated motion code");
        return status;
    }
    /* Spatial copies use reconstructed current-frame pixels; temporal copies
       use the immutable previous frame. */
    source = motion.spatial != 0 ? decoded : previous;
    if (source == NULL || source->pixels == NULL) {
        set_error(error, reader, x, y,
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
        set_error(error, reader, x, y,
                  "motion reference lies outside the frame");
        return REPLAY_MALFORMED_STREAM;
    }

    if (motion.spatial != 0) {
        for (row = 0U; row < (unsigned)block_size; ++row) {
            unsigned column;

            for (column = 0U; column < (unsigned)block_size; ++column) {
                if (!spatial_pixel_available(
                        x, y, block_size,
                        (unsigned)source_x + column,
                        (unsigned)source_y + row)) {
                    set_error(error, reader, x, y,
                              "spatial reference has not been reconstructed");
                    return REPLAY_MALFORMED_STREAM;
                }
            }
        }
    }

    for (row = 0U; row < (unsigned)block_size; ++row) {
        unsigned column;

        for (column = 0U; column < (unsigned)block_size; ++column) {
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

static void report(MbDecodeTrace trace, void *opaque,
                   unsigned x, unsigned y, unsigned size, MbDecodeMode mode,
                   size_t bit_start, size_t bit_end,
                   const MbMotionVector *motion)
{
    MbDecodeEvent event;

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

static ReplayStatus verify_2x2(
    const MbFrameVerifyCodec *codec, ReplayBitReader *reader,
    MbPredictor *predictor, const MbFrame *previous, MbFrame *decoded,
    unsigned x, unsigned y, MbVerifyError *error,
    MbDecodeTrace trace, void *trace_opaque)
{
    ReplayBitReader start = *reader;
    size_t bit_start = replay_bitreader_position(reader);
    uint32_t first;
    ReplayStatus status = replay_bitreader_read(reader, 1U, &first);

    /* `1` is motion; `00` is stationary; `01` is a complete data header. */
    if (status != REPLAY_OK) {
        set_error(error, reader, x, y, "truncated 2x2 opcode");
        return status;
    }
    if (first != 0U) {
        MbMotionVector motion;

        status = copy_motion(reader, MB_MOTION_BLOCK_2X2, previous, decoded,
                             x, y, error, &motion);
        if (status == REPLAY_OK) {
            report(trace, trace_opaque, x, y, 2U,
                   motion.spatial != 0 ? MB_DECODE_MODE_SPATIAL
                                       : MB_DECODE_MODE_TEMPORAL,
                   bit_start, replay_bitreader_position(reader), &motion);
        }
        return status;
    }

    {
        uint32_t second;

        status = replay_bitreader_read(reader, 1U, &second);
        if (status != REPLAY_OK) {
            set_error(error, reader, x, y, "truncated 2x2 opcode");
            return status;
        }
        if (second == 0U) {
            status = copy_stationary(previous, decoded, x, y, 2U,
                                     reader, error);
            if (status == REPLAY_OK) {
                report(trace, trace_opaque, x, y, 2U,
                       MB_DECODE_MODE_STATIONARY, bit_start,
                       replay_bitreader_position(reader), NULL);
            }
            return status;
        }
    }

    /* Restore the prefix because the codec decoder consumes the full header. */
    *reader = start;
    status = codec->decode_data2x2(
        reader, predictor, decoded->pixels + y * decoded->stride + x,
        decoded->stride, error);
    if (status != REPLAY_OK && error != NULL) {
        error->block_x = x;
        error->block_y = y;
    }
    if (status == REPLAY_OK) {
        report(trace, trace_opaque, x, y, 2U, MB_DECODE_MODE_DATA,
               bit_start, replay_bitreader_position(reader), NULL);
    }
    return status;
}

ReplayStatus mb_frame_verify(
    const MbFrameVerifyCodec *codec,
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    MbDecodeTrace trace, void *trace_opaque)
{
    ReplayBitReader reader;
    MbPredictor predictor = { 0U };
    unsigned y;

    if (codec == NULL || codec->decode_data4x4 == NULL ||
        codec->decode_data2x2 == NULL ||
        (payload == NULL && payload_size != 0U) || decoded == NULL ||
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

    /* Scan order is normative for both spatial availability and prediction. */
    for (y = 0U; y < decoded->height; y += 4U) {
        unsigned x;

        for (x = 0U; x < decoded->width; x += 4U) {
            ReplayBitReader start = reader;
            size_t bit_start = replay_bitreader_position(&reader);
            uint32_t opcode;
            ReplayStatus status = replay_bitreader_read(&reader, 2U, &opcode);

            if (status != REPLAY_OK) {
                set_error(error, &reader, x, y, "truncated 4x4 opcode");
                return status;
            }
            switch (opcode) {
            case 0U:
                status = copy_stationary(previous, decoded, x, y, 4U,
                                         &reader, error);
                if (status == REPLAY_OK) {
                    report(trace, trace_opaque, x, y, 4U,
                           MB_DECODE_MODE_STATIONARY, bit_start,
                           replay_bitreader_position(&reader), NULL);
                }
                break;
            case 1U:
                reader = start;
                status = codec->decode_data4x4(
                    &reader, &predictor,
                    decoded->pixels + y * decoded->stride + x,
                    decoded->stride, error);
                if (status != REPLAY_OK && error != NULL) {
                    error->block_x = x;
                    error->block_y = y;
                }
                if (status == REPLAY_OK) {
                    report(trace, trace_opaque, x, y, 4U,
                           MB_DECODE_MODE_DATA, bit_start,
                           replay_bitreader_position(&reader), NULL);
                }
                break;
            case 2U: {
                MbMotionVector motion;

                status = copy_motion(&reader, MB_MOTION_BLOCK_4X4,
                                     previous, decoded, x, y, error, &motion);
                if (status == REPLAY_OK) {
                    report(trace, trace_opaque, x, y, 4U,
                           motion.spatial != 0 ? MB_DECODE_MODE_SPATIAL
                                               : MB_DECODE_MODE_TEMPORAL,
                           bit_start, replay_bitreader_position(&reader),
                           &motion);
                }
                break;
            }
            case 3U:
                status = verify_2x2(codec, &reader, &predictor, previous,
                                    decoded, x, y, error, trace, trace_opaque);
                if (status == REPLAY_OK) {
                    status = verify_2x2(codec, &reader, &predictor, previous,
                                        decoded, x + 2U, y, error,
                                        trace, trace_opaque);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(codec, &reader, &predictor, previous,
                                        decoded, x, y + 2U, error,
                                        trace, trace_opaque);
                }
                if (status == REPLAY_OK) {
                    status = verify_2x2(codec, &reader, &predictor, previous,
                                        decoded, x + 2U, y + 2U, error,
                                        trace, trace_opaque);
                }
                if (status == REPLAY_OK) {
                    report(trace, trace_opaque, x, y, 4U,
                           MB_DECODE_MODE_SPLIT, bit_start,
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

        /* Strict framing catches wrong dimensions and concatenated payloads. */
        if (used_bytes != payload_size) {
            set_error(error, &reader, decoded->width - 4U,
                      decoded->height - 4U, "payload has trailing bytes");
            return REPLAY_MALFORMED_STREAM;
        }
        if ((used_bits & 7U) != 0U &&
            (payload[used_bits / 8U] >> (used_bits & 7U)) != 0U) {
            set_error(error, &reader, decoded->width - 4U,
                      decoded->height - 4U,
                      "payload has non-zero padding bits");
            return REPLAY_MALFORMED_STREAM;
        }
    }
    return REPLAY_OK;
}
