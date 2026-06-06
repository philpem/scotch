#ifndef REPLAY_BITSTREAM_H
#define REPLAY_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

typedef struct {
    ReplayBuffer *buffer;
    uint64_t accumulator;
    unsigned bit_count;
    size_t total_bits;
} ReplayBitWriter;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    uint64_t accumulator;
    unsigned bit_count;
    size_t total_bits;
} ReplayBitReader;

void replay_bitwriter_init(ReplayBitWriter *writer, ReplayBuffer *buffer);
ReplayStatus replay_bitwriter_write(ReplayBitWriter *writer, uint32_t value,
                                    unsigned count);
ReplayStatus replay_bitwriter_flush_zero(ReplayBitWriter *writer);
size_t replay_bitwriter_position(const ReplayBitWriter *writer);

void replay_bitreader_init(ReplayBitReader *reader, const uint8_t *data,
                           size_t size);
ReplayStatus replay_bitreader_read(ReplayBitReader *reader, unsigned count,
                                   uint32_t *value);
size_t replay_bitreader_position(const ReplayBitReader *reader);

#endif

