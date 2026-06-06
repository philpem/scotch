#ifndef MB_FRAME_H
#define MB_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t y;
    uint8_t u;
    uint8_t v;
} MbPixel;

typedef struct {
    uint8_t luma;
} MbPredictor;

typedef struct {
    unsigned width;
    unsigned height;
    size_t stride;
    MbPixel *pixels;
} MbFrame;

typedef struct {
    size_t bit_position;
    unsigned block_x;
    unsigned block_y;
    const char *detail;
} MbVerifyError;

#endif
