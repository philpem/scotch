/*
 * escape130_tables.h -- constant tables shared by the Escape 130 decoder and
 * encoder (spec § 6/8): the signed luma deltas, the packed chroma deltas, and the
 * 64 texture sign tuples, plus a helper to pack the tuples into the block word's
 * texture bits (8..15).
 */
#ifndef ESCAPE130_TABLES_H
#define ESCAPE130_TABLES_H
#include <stdint.h>

static const int ESC130_Y_DIFF[8] = { -4, -3, -2, -1, 1, 2, 3, 4 };

static const uint32_t ESC130_C_DIFF[8] = {
    0x00010000u, 0x01010000u, 0x01000000u, 0x00FF0000u,
    0xFFFF0000u, 0xFEFF0000u, 0xFF000000u, 0xFF010000u
};

/* The four signs (TL,TR,BL,BR) for each 6-bit sidx. */
static const int8_t ESC130_SIGN_TUPLES[64][4] = {
    { 0, 0, 0, 0}, {-1,+1, 0, 0}, {+1,-1, 0, 0}, {-1, 0,+1, 0},
    {-1,+1,+1, 0}, { 0,-1,+1, 0}, {+1,-1,+1, 0}, {-1,-1,+1, 0},
    {+1, 0,-1, 0}, { 0,+1,-1, 0}, {+1,+1,-1, 0}, {-1,+1,-1, 0},
    {+1,-1,-1, 0}, {-1, 0, 0,+1}, {-1,+1, 0,+1}, { 0,-1, 0,+1},
    { 0, 0, 0, 0}, {+1,-1, 0,+1}, {-1,-1, 0,+1}, {-1, 0,+1,+1},
    {-1,+1,+1,+1}, { 0,-1,+1,+1}, {+1,-1,+1,+1}, {-1,-1,+1,+1},
    { 0, 0,-1,+1}, {+1, 0,-1,+1}, {-1, 0,-1,+1}, { 0,+1,-1,+1},
    {+1,+1,-1,+1}, {-1,+1,-1,+1}, { 0,-1,-1,+1}, {+1,-1,-1,+1},
    { 0, 0, 0, 0}, {-1,-1,-1,+1}, {+1, 0, 0,-1}, { 0,+1, 0,-1},
    {+1,+1, 0,-1}, {-1,+1, 0,-1}, {+1,-1, 0,-1}, { 0, 0,+1,-1},
    {+1, 0,+1,-1}, {-1, 0,+1,-1}, { 0,+1,+1,-1}, {+1,+1,+1,-1},
    {-1,+1,+1,-1}, { 0,-1,+1,-1}, {+1,-1,+1,-1}, {-1,-1,+1,-1},
    { 0, 0, 0, 0}, {+1, 0,-1,-1}, { 0,+1,-1,-1}, {+1,+1,-1,-1},
    {-1,+1,-1,-1}, {+1,-1,-1,-1}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}
};

/* Pack the sign tuples into each entry's texture bits (8..15): a +1 sign is code
 * 1, -1 is code 2, 0 is code 0, at 2 bits per sub-pixel. */
static void esc130_build_sign_tbl(uint32_t out[64])
{
    int si, k;
    for (si = 0; si < 64; si++) {
        uint32_t entry = 0;
        for (k = 0; k < 4; k++) {
            int sgn = ESC130_SIGN_TUPLES[si][k];
            unsigned code = (sgn > 0) ? 1u : (sgn < 0) ? 2u : 0u;
            entry |= code << (8 + 2 * k);
        }
        out[si] = entry;
    }
}

#endif /* ESCAPE130_TABLES_H */
