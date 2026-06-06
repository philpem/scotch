#ifndef MB_HUFFMAN_H
#define MB_HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_bitstream.h"

typedef struct {
    /* Low `bit_count` bits, in the LSB-first order stored by Replay. */
    uint16_t bits;
    uint8_t bit_count;
} MbHuffmanCode;

/* Direct symbol-to-code table; the array index is the decoded symbol. */
typedef struct {
    const MbHuffmanCode *codes;
    size_t symbol_count;
    uint8_t max_bits;
} MbHuffmanTable;

ReplayStatus mb_huffman_write(ReplayBitWriter *writer,
                              const MbHuffmanTable *table,
                              unsigned symbol);
ReplayStatus mb_huffman_read(ReplayBitReader *reader,
                             const MbHuffmanTable *table,
                             unsigned *symbol);
ReplayStatus mb_huffman_validate(const MbHuffmanTable *table);

#endif
