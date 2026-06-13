#ifndef MB_FRAME_H
#define MB_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Quantised luma. Format 19 uses only values 0..63. */
    uint8_t y;
    /*
     * Quantised chroma stored in its on-wire modulo representation. For
     * format 19, 0..15 mean 0..+15 and 16..31 mean -16..-1.
     */
    uint8_t u;
    uint8_t v;
} MbPixel;

typedef struct {
    /* Average luma of the most recent data-coded block, initially zero. */
    uint8_t luma;
    /* Type 20 "new" (delta-coded chroma) only: the previous data block's 6-bit
       U and V, carried like luma and reset to zero each frame. Unused (zero) by
       every other codec, including type 20 "old". */
    uint8_t chroma_u;
    uint8_t chroma_v;
} MbPredictor;

typedef struct {
    unsigned width;
    unsigned height;
    /* Distance between rows in MbPixel elements, not bytes. */
    size_t stride;
    MbPixel *pixels;
} MbFrame;

/* Location and explanation of the first payload verification failure. */
typedef struct {
    size_t bit_position;
    unsigned block_x;
    unsigned block_y;
    const char *detail;
} MbVerifyError;

#endif
