/* replay/codecif.h -- run an Acorn Replay "CodecIf" decompressor module.
 *
 * This is the reusable layer between the raw ARM sandbox (replay/armsim.h) and
 * the tools. It loads a compiled ARMovie decompressor (the three-word CodecIf
 * header: patch-table offset, init branch, decompress branch), lays out the
 * standard memory map, runs the init entry once, and then decodes frames one at
 * a time -- maintaining the alternating reconstruction buffers so each decoded
 * frame becomes the temporal reference for the next, exactly as a Replay player
 * does.
 *
 * Both the decoder cross-check tool (replay-armsim) and a future Replay->raw
 * transcoder sit on top of this, so the per-frame contract and the pixel-layout
 * conversions live here rather than being duplicated per tool.
 *
 * The decompressor is left unpatched (its colour-lookup patch table untouched),
 * so each output word carries the codec's native packed components rather than
 * a screen-format pixel; replay_pix_unpack() turns those words into byte
 * triplets in whichever layout the codec uses.
 */

#ifndef REPLAY_CODECIF_H
#define REPLAY_CODECIF_H

#include <stddef.h>
#include <stdint.h>

#include "replay/armsim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Component packing of a decompressor's working words.
 *
 * On the byte side each pixel is three bytes (Y, U, V); on the ARM side it is
 * one 32-bit word with codec-specific bit fields. YUV555_TO_6Y5UV is an output
 * conversion only: it reads native type-7 YUV555 words and emits 6Y5UV byte
 * triplets (the type-19 working layout). RAW16 is the Moving Lines case: each
 * pixel is a native little-endian 16-bit value, expanded to/from one word. */
typedef enum {
    REPLAY_PIX_6Y5UV = 0,       /* word: Y[0:5] U[6:10] V[11:15]  (type 19/23) */
    REPLAY_PIX_YUV555,          /* word: Y[0:4] U[5:9] V[10:14]   (type 7/17)  */
    REPLAY_PIX_6Y6UV,           /* word: Y[0:5] U[6:11] V[12:17]  (type 20)    */
    REPLAY_PIX_YUV555_TO_6Y5UV, /* read YUV555 words, write 6Y5UV bytes        */
    REPLAY_PIX_RAW16            /* native 16-bit pixel <-> low halfword of word */
} ReplayPixelLayout;

/* Bytes per packed pixel for a layout (3 for the YUV triplet layouts, 2 for
 * RAW16). */
size_t replay_pix_bytes_per_pixel(ReplayPixelLayout layout);

/* Convert packed component bytes <-> ARM working words (4 bytes/pixel).
 * `pack` rejects out-of-range components. YUV555_TO_6Y5UV is unpack-only.
 * Return 0 on success, -1 on error (with *bad_pixel set when non-NULL). */
int replay_pix_pack(ReplayPixelLayout layout, const uint8_t *bytes,
                    size_t pixel_count, uint8_t *words_out, size_t *bad_pixel);
int replay_pix_unpack(ReplayPixelLayout layout, const uint8_t *words,
                      size_t pixel_count, uint8_t *bytes_out);

/* An open decoder session. */
typedef struct ReplayCodecIf ReplayCodecIf;

/* Load `module` (a compiled CodecIf Decompress file), lay out the memory map,
 * and run the init entry with the given dimensions. `mode` selects the ARM
 * model (use REPLAY_ARM_MODE_32 to match the historical Unicorn harness).
 * Returns NULL on failure, writing a message to errbuf when non-NULL. */
ReplayCodecIf *replay_codecif_open(const uint8_t *module, size_t module_len,
                                   unsigned width, unsigned height,
                                   ReplayArmMode mode,
                                   char *errbuf, size_t errlen);
void replay_codecif_close(ReplayCodecIf *cif);

/* Frame size in ARM working words (width*height*4 bytes). */
size_t replay_codecif_frame_words_len(const ReplayCodecIf *cif);

/* Load the video payload to be decoded (mapped once; frames are decoded from it
 * sequentially by replay_codecif_decode). Returns 0 / -1. */
int replay_codecif_load_payload(ReplayCodecIf *cif,
                                const uint8_t *payload, size_t payload_len,
                                char *errbuf, size_t errlen);

/* Seed the temporal reference for the next decode from ARM working words
 * (frame_words_len bytes). Skip for a key frame; buffers start zeroed. */
int replay_codecif_set_previous_words(ReplayCodecIf *cif,
                                      const uint8_t *words, size_t len,
                                      char *errbuf, size_t errlen);

/* Decode one frame starting at payload offset *offset. On success out_words
 * receives the decoded frame as ARM working words (frame_words_len bytes), the
 * decoded frame becomes the next temporal reference, *consumed is the frame's
 * byte length, and *offset is advanced past it. Returns 0 / -1. */
int replay_codecif_decode(ReplayCodecIf *cif, size_t *offset,
                          uint8_t *out_words, size_t *consumed,
                          char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_CODECIF_H */
