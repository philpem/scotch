#include "replay/mb_repeat.h"

/*
 * Bits consumed by one stationary block. Types 17/19/20 share the HQ grammar: a
 * 2-bit opcode (00 = stationary), nothing more. Type 7's grammar reads a 1-bit
 * "data?" flag (0), a 1-bit move/split selector (0 = move), then a 2-bit move
 * family (00 = stationary): four zero bits in total. All are zero, so the whole
 * frame is a run of zero bytes.
 */
static unsigned stationary_block_bits(unsigned codec)
{
    switch (codec) {
    case 7U:
        return 4U;
    case 17U:
    case 19U:
    case 20U:
        return 2U;
    default:
        return 0U;
    }
}

ReplayStatus mb_repeat_payload(unsigned codec, unsigned width, unsigned height,
                               ReplayBuffer *out)
{
    unsigned block_bits = stationary_block_bits(codec);
    unsigned blocks_x;
    unsigned blocks_y;
    size_t total_bits;
    size_t total_bytes;
    size_t i;

    if (width == 0U || height == 0U) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (block_bits == 0U) {
        return REPLAY_UNSUPPORTED_CODEC;
    }

    /* Blocks are scanned on a 4x4 grid; a frame whose dimensions are not exact
     * multiples of four still emits one block per partial 4x4 cell. */
    blocks_x = (width + 3U) / 4U;
    blocks_y = (height + 3U) / 4U;
    total_bits = (size_t)blocks_x * blocks_y * block_bits;
    total_bytes = (total_bits + 7U) / 8U;

    replay_buffer_clear(out);
    for (i = 0U; i < total_bytes; ++i) {
        ReplayStatus status = replay_buffer_append_u8(out, 0U);

        if (status != REPLAY_OK) {
            return status;
        }
    }
    return REPLAY_OK;
}
