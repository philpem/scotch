#include "replay/mb_huffman.h"

#include <limits.h>

/*
 * Replay's Huffman tables are recorded as the bit patterns seen by its
 * LSB-first reader. They must not be reversed into the MSB-first notation
 * commonly used when documenting canonical Huffman codes.
 *
 * This decoder intentionally performs a linear table search. The format-19
 * alphabet has only 64 symbols, and a simple implementation is easier to
 * audit against the original table. A faster lookup table can be added later
 * without changing this representation or its tests.
 */

static uint16_t low_mask(unsigned count)
{
    if (count == 16U) {
        return (uint16_t)UINT16_MAX;
    }
    return (uint16_t)((UINT32_C(1) << count) - UINT32_C(1));
}

ReplayStatus mb_huffman_write(ReplayBitWriter *writer,
                              const MbHuffmanTable *table,
                              unsigned symbol)
{
    const MbHuffmanCode *code;

    if (writer == NULL || table == NULL || table->codes == NULL ||
        symbol >= table->symbol_count) {
        return REPLAY_INVALID_ARGUMENT;
    }
    code = &table->codes[symbol];
    if (code->bit_count == 0U || code->bit_count > table->max_bits) {
        return REPLAY_MALFORMED_STREAM;
    }
    return replay_bitwriter_write(writer, code->bits, code->bit_count);
}

ReplayStatus mb_huffman_read(ReplayBitReader *reader,
                             const MbHuffmanTable *table,
                             unsigned *symbol)
{
    uint16_t bits = 0;
    unsigned length;

    if (reader == NULL || table == NULL || table->codes == NULL ||
        symbol == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }

    /* A prefix can only be recognized after each successive stream bit. */
    for (length = 1U; length <= table->max_bits; ++length) {
        uint32_t bit;
        size_t candidate;
        ReplayStatus status = replay_bitreader_read(reader, 1U, &bit);
        if (status != REPLAY_OK) {
            return status;
        }
        bits |= (uint16_t)(bit << (length - 1U));

        for (candidate = 0; candidate < table->symbol_count; ++candidate) {
            const MbHuffmanCode *code = &table->codes[candidate];
            if (code->bit_count == length && code->bits == bits) {
                *symbol = (unsigned)candidate;
                return REPLAY_OK;
            }
        }
    }
    return REPLAY_MALFORMED_STREAM;
}

ReplayStatus mb_huffman_validate(const MbHuffmanTable *table)
{
    size_t first;

    if (table == NULL || table->codes == NULL || table->symbol_count == 0U ||
        table->max_bits == 0U || table->max_bits > 16U) {
        return REPLAY_INVALID_ARGUMENT;
    }

    for (first = 0; first < table->symbol_count; ++first) {
        const MbHuffmanCode *a = &table->codes[first];
        size_t second;

        if (a->bit_count == 0U || a->bit_count > table->max_bits ||
            (a->bits & (uint16_t)~low_mask(a->bit_count)) != 0U) {
            return REPLAY_MALFORMED_STREAM;
        }
        /* No complete code may be the low-bit prefix of another code. */
        for (second = first + 1U; second < table->symbol_count; ++second) {
            const MbHuffmanCode *b = &table->codes[second];
            const MbHuffmanCode *shorter = a;
            const MbHuffmanCode *longer = b;

            if (b->bit_count < a->bit_count) {
                shorter = b;
                longer = a;
            }
            if ((longer->bits & low_mask(shorter->bit_count)) ==
                shorter->bits) {
                return REPLAY_MALFORMED_STREAM;
            }
        }
    }
    return REPLAY_OK;
}
