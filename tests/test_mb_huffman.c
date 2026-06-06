#include "test_common.h"

#include <stdint.h>

#include "replay/codec_supermovingblocks.h"

static int test_all_symbols(void)
{
    const MbHuffmanTable *table = &codec_supermovingblocks_luma_huffman;
    unsigned expected;

    CHECK(mb_huffman_validate(table) == REPLAY_OK);
    for (expected = 0; expected < table->symbol_count; ++expected) {
        ReplayBuffer buffer;
        ReplayBitWriter writer;
        ReplayBitReader reader;
        unsigned actual = 999U;

        replay_buffer_init(&buffer);
        replay_bitwriter_init(&writer, &buffer);
        CHECK(mb_huffman_write(&writer, table, expected) == REPLAY_OK);
        CHECK(replay_bitwriter_position(&writer) ==
              table->codes[expected].bit_count);
        CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

        replay_bitreader_init(&reader, buffer.data, buffer.size);
        CHECK(mb_huffman_read(&reader, table, &actual) == REPLAY_OK);
        CHECK(actual == expected);
        CHECK(replay_bitreader_position(&reader) ==
              table->codes[expected].bit_count);
        replay_buffer_free(&buffer);
    }
    return EXIT_SUCCESS;
}

static int test_source_derived_golden_bytes(void)
{
    const MbHuffmanTable *table = &codec_supermovingblocks_luma_huffman;
    ReplayBuffer buffer;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    unsigned symbol;

    replay_buffer_init(&buffer);
    replay_bitwriter_init(&writer, &buffer);

    /* Source codes: 0=%10, 1=%111, 63=%011. LSB-first gives 0x7e. */
    CHECK(mb_huffman_write(&writer, table, 0U) == REPLAY_OK);
    CHECK(mb_huffman_write(&writer, table, 1U) == REPLAY_OK);
    CHECK(mb_huffman_write(&writer, table, 63U) == REPLAY_OK);
    CHECK(replay_bitwriter_position(&writer) == 8U);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(buffer.size == 1U);
    CHECK(buffer.data[0] == UINT8_C(0x7e));

    replay_bitreader_init(&reader, buffer.data, buffer.size);
    CHECK(mb_huffman_read(&reader, table, &symbol) == REPLAY_OK);
    CHECK(symbol == 0U);
    CHECK(mb_huffman_read(&reader, table, &symbol) == REPLAY_OK);
    CHECK(symbol == 1U);
    CHECK(mb_huffman_read(&reader, table, &symbol) == REPLAY_OK);
    CHECK(symbol == 63U);

    replay_buffer_free(&buffer);
    return EXIT_SUCCESS;
}

static int test_truncated_symbol(void)
{
    const uint8_t partial = UINT8_C(0x00);
    ReplayBitReader reader;
    unsigned symbol;

    /* Five zero bits are symbol 59, so consume three first to leave too few. */
    replay_bitreader_init(&reader, &partial, 1U);
    reader.byte_pos = 1U;
    reader.accumulator = 0U;
    reader.bit_count = 3U;
    CHECK(mb_huffman_read(&reader, &codec_supermovingblocks_luma_huffman,
                          &symbol) == REPLAY_TRUNCATED_INPUT);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_all_symbols() == EXIT_SUCCESS);
    CHECK(test_source_derived_golden_bytes() == EXIT_SUCCESS);
    CHECK(test_truncated_symbol() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

