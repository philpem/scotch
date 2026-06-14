#ifndef CODEC_MOVINGLINES_H
#define CODEC_MOVINGLINES_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

/*
 * Compression type 1, Moving Lines (see docs/spec/type1-moving-lines.md).
 *
 * Moving Lines is a line-oriented codec over opaque 15-bit pixels (YUV555 or
 * RGB555 per the movie's declared colour; this codec does not interpret them).
 * A frame is a halfword command stream; pixels are addressed in raster order.
 * Here a frame's pixels are a width*height array of uint16_t, each holding a
 * 15-bit value (bit 15 must be zero).
 *
 * The decoder implements the whole command grammar (literals, temporal and
 * spatial copies, the same-position previous-frame copy, the bit-packed long
 * literal run, repeated-pixel runs and end-of-frame). The encoder emits a
 * valid, byte-exact-decodable subset (same-position copies, repeated-pixel runs
 * and literals); it does not search temporal/spatial offsets, so it favours
 * correctness and round-trip fidelity over Acorn's compression ratio.
 */

/* Largest 15-bit pixel value (bit 15 is reserved as the literal flag on the
   wire and must be zero in a stored pixel). */
#define CODEC_MOVINGLINES_PIXEL_MAX 0x7FFFU

/*
 * Decode one frame. `payload`/`payload_size` is the halfword command stream;
 * `previous` is the previous reconstructed frame (NULL for a key frame with no
 * temporal references); `decoded` receives width*height reconstructed pixels.
 * `*consumed` (optional) returns the number of payload bytes read, which is
 * halfword-aligned. A temporal/spatial copy that references outside the frame,
 * or a temporal copy with no previous frame, is REPLAY_MALFORMED_STREAM.
 */
ReplayStatus codec_movinglines_decode_frame(const uint8_t *payload,
                                            size_t payload_size,
                                            const uint16_t *previous,
                                            uint16_t *decoded, unsigned width,
                                            unsigned height, size_t *consumed);

/*
 * Encode one frame to `out` (cleared on entry). `source` is width*height 15-bit
 * pixels; `previous` is the previous reconstructed frame, or NULL for a key
 * frame. Output is a complete Moving Lines frame ending in the end-of-frame
 * halfword, and decodes back to `source` byte-for-byte.
 */
ReplayStatus codec_movinglines_encode_frame(const uint16_t *source,
                                            const uint16_t *previous,
                                            unsigned width, unsigned height,
                                            ReplayBuffer *out);

#endif
