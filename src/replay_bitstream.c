#include "replay/replay_bitstream.h"

#include <limits.h>

/*
 * Replay writes fields least-significant bit first. The accumulator therefore
 * keeps the next stream bit at bit zero; complete low bytes can be emitted or
 * consumed without reversing individual fields.
 */

void replay_bitwriter_init(ReplayBitWriter *writer, ReplayBuffer *buffer)
{
    if (writer == NULL) {
        return;
    }
    writer->buffer = buffer;
    writer->accumulator = 0;
    writer->bit_count = 0;
    writer->total_bits = 0;
}

ReplayStatus replay_bitwriter_write(ReplayBitWriter *writer, uint32_t value,
                                    unsigned count)
{
    uint64_t masked;
    ReplayStatus status;

    if (writer == NULL || writer->buffer == NULL || count > 32U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (count == 0U) {
        return REPLAY_OK;
    }
    if (writer->total_bits > SIZE_MAX - count) {
        return REPLAY_INVALID_ARGUMENT;
    }

    /* Ignore high caller bits rather than allowing them into the next field. */
    masked = value;
    if (count < 32U) {
        masked &= (UINT64_C(1) << count) - UINT64_C(1);
    }
    writer->accumulator |= masked << writer->bit_count;
    writer->bit_count += count;
    writer->total_bits += count;

    while (writer->bit_count >= 8U) {
        status = replay_buffer_append_u8(writer->buffer,
                                         (uint8_t)writer->accumulator);
        if (status != REPLAY_OK) {
            return status;
        }
        writer->accumulator >>= 8U;
        writer->bit_count -= 8U;
    }
    return REPLAY_OK;
}

ReplayStatus replay_bitwriter_flush_zero(ReplayBitWriter *writer)
{
    ReplayStatus status;

    if (writer == NULL || writer->buffer == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (writer->bit_count == 0U) {
        return REPLAY_OK;
    }
    /* Unused high bits in the final byte are required to be zero. */
    status = replay_buffer_append_u8(writer->buffer,
                                     (uint8_t)writer->accumulator);
    if (status != REPLAY_OK) {
        return status;
    }
    writer->accumulator = 0;
    writer->bit_count = 0;
    return REPLAY_OK;
}

size_t replay_bitwriter_position(const ReplayBitWriter *writer)
{
    return writer == NULL ? 0U : writer->total_bits;
}

void replay_bitreader_init(ReplayBitReader *reader, const uint8_t *data,
                           size_t size)
{
    if (reader == NULL) {
        return;
    }
    reader->data = data;
    reader->size = size;
    reader->byte_pos = 0;
    reader->accumulator = 0;
    reader->bit_count = 0;
    reader->total_bits = 0;
}

ReplayStatus replay_bitreader_read(ReplayBitReader *reader, unsigned count,
                                   uint32_t *value)
{
    uint64_t mask;

    if (reader == NULL || value == NULL || count > 32U ||
        (reader->data == NULL && reader->size != 0U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (count == 0U) {
        *value = 0;
        return REPLAY_OK;
    }

    /* Refill above existing bits because the low bits are consumed first. */
    while (reader->bit_count < count) {
        if (reader->byte_pos >= reader->size) {
            return REPLAY_TRUNCATED_INPUT;
        }
        reader->accumulator |=
            (uint64_t)reader->data[reader->byte_pos++] << reader->bit_count;
        reader->bit_count += 8U;
    }

    mask = count == 32U ? UINT32_MAX
                        : (UINT64_C(1) << count) - UINT64_C(1);
    *value = (uint32_t)(reader->accumulator & mask);
    reader->accumulator >>= count;
    reader->bit_count -= count;
    reader->total_bits += count;
    return REPLAY_OK;
}

size_t replay_bitreader_position(const ReplayBitReader *reader)
{
    return reader == NULL ? 0U : reader->total_bits;
}
