#ifndef REPLAY_MB_REPEAT_H
#define REPLAY_MB_REPEAT_H

#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

/*
 * Build the "repeat last frame" payload for a Moving Blocks codec (7/17/19/20):
 * a frame in which every 4x4 block carries the stationary opcode, which copies
 * the previous reconstruction unchanged. The stationary opcode is all-zero bits
 * in every Moving Blocks grammar (2 bits for 17/19/20, 4 bits for type 7), so
 * the payload is simply a run of zero bytes long enough to cover one opcode per
 * block. Verified to decode identically to the previous frame on the real Acorn
 * Decomp7/17/19/20 modules.
 *
 * Used to pad a movie's final chunk up to frames-per-chunk so the player (which
 * decodes a fixed number of frames per chunk) does not run off the end into
 * garbage -- the same "repeating last frame" padding Acorn's own compressor
 * applies.
 *
 * Returns REPLAY_OK with the payload appended to `out` (which is cleared first),
 * REPLAY_INVALID_ARGUMENT for zero dimensions, REPLAY_UNSUPPORTED_CODEC for a
 * codec with no known stationary encoding, or REPLAY_OUT_OF_MEMORY.
 */
ReplayStatus mb_repeat_payload(unsigned codec, unsigned width, unsigned height,
                               ReplayBuffer *out);

#endif /* REPLAY_MB_REPEAT_H */
