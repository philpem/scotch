#include "replay/mb_motion.h"

#include <stddef.h>

/*
 * Spatial references must point wholly into pixels earlier in raster order.
 * The legal offsets differ for 4x4 and 2x2 blocks; these tables are part of
 * the format rather than an encoder search policy. They are shared by types 7,
 * 17 and 19 and were taken from the running decoder, not the docs: see the
 * note above mb_motion_read_format7 -- the published 2x2 column is in a
 * scrambled order, so the compiled Decomp module is the authority.
 */
static const MbMotionVector spatial_4x4[8] = {
    { -2, -4, 1 }, { -1, -4, 1 }, { 0, -4, 1 }, { 1, -4, 1 },
    { 2, -4, 1 },  { -4, 0, 1 },  { -4, -1, 1 }, { -4, -2, 1 }
};

static const MbMotionVector spatial_2x2[8] = {
    { -2, -2, 1 }, { -1, -2, 1 }, { 0, -2, 1 }, { 1, -2, 1 },
    { 2, -2, 1 },  { -2, -1, 1 }, { -2, 0, 1 },  { -3, 0, 1 }
};

static MbMotionVector ring_vector(unsigned radius, unsigned index)
{
    MbMotionVector result = { 0, 0, 0 };
    unsigned side = radius * 2U + 1U;

    /* Table order is top edge, alternating left/right edges, then bottom. */
    if (index < side) {
        result.dx = (int)index - (int)radius;
        result.dy = -(int)radius;
    } else if (index < side + (radius * 2U - 1U) * 2U) {
        unsigned side_index = index - side;
        result.dy = (int)(side_index / 2U) - (int)radius + 1;
        result.dx = (side_index & 1U) == 0U ? -(int)radius : (int)radius;
    } else {
        unsigned bottom_index =
            index - side - (radius * 2U - 1U) * 2U;
        result.dx = (int)bottom_index - (int)radius;
        result.dy = (int)radius;
    }
    return result;
}

static ReplayStatus far_vector(unsigned index, MbMotionVector *motion)
{
    unsigned current = 0;
    int dy;

    for (dy = -8; dy <= 8; ++dy) {
        int dx;
        for (dx = -8; dx <= 8; ++dx) {
            /* Radii 1-3 have shorter families and are omitted from `far`. */
            if (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3) {
                continue;
            }
            if (current == index) {
                motion->dx = dx;
                motion->dy = dy;
                motion->spatial = 0;
                return REPLAY_OK;
            }
            ++current;
        }
    }
    return REPLAY_MALFORMED_STREAM;
}

ReplayStatus mb_motion_format19_temporal_at(unsigned index,
                                            MbMotionVector *motion)
{
    if (motion == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /* Increasing index also means non-decreasing encoded bit length. */
    if (index < 8U) {
        *motion = ring_vector(1U, index);
        return REPLAY_OK;
    }
    if (index < 24U) {
        *motion = ring_vector(2U, index - 8U);
        return REPLAY_OK;
    }
    if (index < 48U) {
        *motion = ring_vector(3U, index - 24U);
        return REPLAY_OK;
    }
    if (index < 288U) {
        return far_vector(index - 48U, motion);
    }
    return REPLAY_INVALID_ARGUMENT;
}

ReplayStatus mb_motion_format19_spatial_at(MbMotionBlockSize block_size,
                                           unsigned index,
                                           MbMotionVector *motion)
{
    if (motion == NULL || index >= 8U ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    *motion = block_size == MB_MOTION_BLOCK_4X4
                  ? spatial_4x4[index]
                  : spatial_2x2[index];
    return REPLAY_OK;
}

/*
 * Type 7's spatial tables (Decomp7 indices 56..63) are identical to the 17/19
 * tables -- confirmed by probing the compiled Decomp7. (Docs/Stream lists the
 * 2x2 column in a scrambled order; the running decoder is the authority.)
 */

ReplayStatus mb_motion_read_format7(ReplayBitReader *reader,
                                    MbMotionBlockSize block_size,
                                    MbMotionVector *motion)
{
    uint32_t first;
    uint32_t second;
    uint32_t index;
    ReplayStatus status;

    if (reader == NULL || motion == NULL ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = replay_bitreader_read(reader, 1U, &first);
    if (status == REPLAY_OK) {
        status = replay_bitreader_read(reader, 1U, &second);
    }
    if (status != REPLAY_OK) {
        return status;
    }
    if (first == 0U) {
        if (second == 0U) {
            /* `00`: temporal copy from the same place. */
            *motion = (MbMotionVector){ 0, 0, 0 };
            return REPLAY_OK;
        }
        /* `01` + 3-bit index: temporal radius 1. */
        status = replay_bitreader_read(reader, 3U, &index);
        if (status != REPLAY_OK) {
            return status;
        }
        *motion = ring_vector(1U, index);
        return REPLAY_OK;
    }
    if (second == 0U) {
        /* `10` + 4-bit index: temporal radius 2. */
        status = replay_bitreader_read(reader, 4U, &index);
        if (status != REPLAY_OK) {
            return status;
        }
        *motion = ring_vector(2U, index);
        return REPLAY_OK;
    }
    /* `11` + 6-bit index: temporal radius 4/3, then spatial. */
    status = replay_bitreader_read(reader, 6U, &index);
    if (status != REPLAY_OK) {
        return status;
    }
    if (index < 32U) {
        *motion = ring_vector(4U, index);
    } else if (index < 56U) {
        *motion = ring_vector(3U, index - 32U);
    } else {
        *motion = block_size == MB_MOTION_BLOCK_4X4
                      ? spatial_4x4[index - 56U]
                      : spatial_2x2[index - 56U];
    }
    return REPLAY_OK;
}

/* Enumerate the type 7 temporal vectors in non-decreasing code length: radius
 * 1 (`01`, 5-bit) then 2 (`10`, 6-bit) then radii 4 and 3 (`11`, 8-bit), the
 * shared encoder's preferred-shortest order. The (0,0) stationary code is the
 * separate 2-bit `00` and is not enumerated here. */
ReplayStatus mb_motion_format7_temporal_at(unsigned index,
                                           MbMotionVector *motion)
{
    if (motion == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (index < 8U) {
        *motion = ring_vector(1U, index);
        return REPLAY_OK;
    }
    if (index < 24U) {
        *motion = ring_vector(2U, index - 8U);
        return REPLAY_OK;
    }
    if (index < 56U) {
        *motion = ring_vector(4U, index - 24U);
        return REPLAY_OK;
    }
    if (index < 80U) {
        *motion = ring_vector(3U, index - 56U);
        return REPLAY_OK;
    }
    return REPLAY_INVALID_ARGUMENT;
}

ReplayStatus mb_motion_format7_spatial_at(MbMotionBlockSize block_size,
                                          unsigned index,
                                          MbMotionVector *motion)
{
    if (motion == NULL || index >= 8U ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    *motion = block_size == MB_MOTION_BLOCK_4X4
                  ? spatial_4x4[index]
                  : spatial_2x2[index];
    return REPLAY_OK;
}

/*
 * Reverse a ring vector to its index in O(1), mirroring ring_vector's order
 * (top edge, then alternating left/right edges, then bottom). Returns 0 unless
 * the vector lies exactly on the radius-r ring (Chebyshev distance r), matching
 * the old linear search's reject-when-not-found contract.
 */
static int ring_index(unsigned radius, const MbMotionVector *motion,
                      unsigned *index)
{
    int r = (int)radius;
    int adx = motion->dx < 0 ? -motion->dx : motion->dx;
    int ady = motion->dy < 0 ? -motion->dy : motion->dy;
    unsigned side = radius * 2U + 1U;

    if (radius == 0U || adx > r || ady > r || (adx != r && ady != r)) {
        return 0;
    }
    if (motion->dy == -r) {
        /* Top edge: dx runs -r..r at index 0..2r. */
        *index = (unsigned)(motion->dx + r);
    } else if (motion->dy == r) {
        /* Bottom edge follows the two side edges. */
        *index = side + (radius * 2U - 1U) * 2U + (unsigned)(motion->dx + r);
    } else {
        /* Side edges interleave: even index is the left column, odd the right. */
        *index = side + (unsigned)((motion->dy + r - 1) * 2) +
                 (motion->dx == r ? 1U : 0U);
    }
    return 1;
}

/*
 * Reverse a `far` vector to its index in O(1), mirroring far_vector's raster
 * scan of the [-8,8] square with the inner [-3,3] box removed. Returns 0 for a
 * vector outside the square or inside the omitted box (i.e. not in the family).
 */
static int far_index(const MbMotionVector *motion, unsigned *index)
{
    int dx = motion->dx;
    int dy = motion->dy;
    int rows_below;
    int inner_before;

    if (dx < -8 || dx > 8 || dy < -8 || dy > 8 ||
        (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3)) {
        return 0;
    }
    /* Subtract the omitted inner cells that precede (dx, dy) in raster order. */
    rows_below = dy + 3;
    if (rows_below < 0) {
        rows_below = 0;
    } else if (rows_below > 7) {
        rows_below = 7;
    }
    inner_before = rows_below * 7;
    if (dy >= -3 && dy <= 3 && dx >= 4) {
        /* On an inner row, all seven omitted cells sit left of dx >= 4. */
        inner_before += 7;
    }
    *index = (unsigned)((dy + 8) * 17 + (dx + 8) - inner_before);
    return 1;
}

/* Reverse a type 7 spatial vector to its table index. */
static int spatial_index_format7(MbMotionBlockSize block_size,
                                 const MbMotionVector *motion, unsigned *index)
{
    const MbMotionVector *table = block_size == MB_MOTION_BLOCK_4X4
                                      ? spatial_4x4
                                      : spatial_2x2;
    unsigned i;

    for (i = 0U; i < 8U; ++i) {
        if (table[i].dx == motion->dx && table[i].dy == motion->dy) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

/*
 * Write a type 7 move code (family then index, least-significant bit first)
 * after the caller has emitted the move opcode. The vector's radius selects the
 * family; spatial vectors take the `11` family indices 56..63.
 */
ReplayStatus mb_motion_write_format7(ReplayBitWriter *writer,
                                     MbMotionBlockSize block_size,
                                     const MbMotionVector *motion)
{
    unsigned index;
    unsigned radius;
    int adx;
    int ady;
    ReplayStatus status;

    if (writer == NULL || motion == NULL ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (motion->spatial != 0) {
        if (!spatial_index_format7(block_size, motion, &index)) {
            return REPLAY_INVALID_ARGUMENT;
        }
        status = replay_bitwriter_write(writer, 3U, 2U); /* `11` */
        return status == REPLAY_OK
                   ? replay_bitwriter_write(writer, 56U + index, 6U)
                   : status;
    }
    adx = motion->dx < 0 ? -motion->dx : motion->dx;
    ady = motion->dy < 0 ? -motion->dy : motion->dy;
    radius = (unsigned)(adx > ady ? adx : ady);
    if (radius == 0U) {
        return replay_bitwriter_write(writer, 0U, 2U); /* `00` stationary */
    }
    if (radius == 1U) {
        if (!ring_index(1U, motion, &index)) {
            return REPLAY_INVALID_ARGUMENT;
        }
        status = replay_bitwriter_write(writer, 2U, 2U); /* `01` */
        return status == REPLAY_OK
                   ? replay_bitwriter_write(writer, index, 3U)
                   : status;
    }
    if (radius == 2U) {
        if (!ring_index(2U, motion, &index)) {
            return REPLAY_INVALID_ARGUMENT;
        }
        status = replay_bitwriter_write(writer, 1U, 2U); /* `10` */
        return status == REPLAY_OK
                   ? replay_bitwriter_write(writer, index, 4U)
                   : status;
    }
    if (radius == 3U) {
        if (!ring_index(3U, motion, &index)) {
            return REPLAY_INVALID_ARGUMENT;
        }
        status = replay_bitwriter_write(writer, 3U, 2U); /* `11` */
        return status == REPLAY_OK
                   ? replay_bitwriter_write(writer, 32U + index, 6U)
                   : status;
    }
    if (radius == 4U) {
        if (!ring_index(4U, motion, &index)) {
            return REPLAY_INVALID_ARGUMENT;
        }
        status = replay_bitwriter_write(writer, 3U, 2U); /* `11` */
        return status == REPLAY_OK
                   ? replay_bitwriter_write(writer, index, 6U)
                   : status;
    }
    return REPLAY_INVALID_ARGUMENT;
}

static int same_vector(const MbMotionVector *left, const MbMotionVector *right)
{
    return left->dx == right->dx && left->dy == right->dy &&
           left->spatial == right->spatial;
}

static ReplayStatus write_index(ReplayBitWriter *writer, unsigned family,
                                unsigned index, unsigned index_bits)
{
    ReplayStatus status = replay_bitwriter_write(writer, family, 2U);

    return status == REPLAY_OK
               ? replay_bitwriter_write(writer, index, index_bits)
               : status;
}

ReplayStatus mb_motion_write_format19(ReplayBitWriter *writer,
                                      MbMotionBlockSize block_size,
                                      const MbMotionVector *motion)
{
    unsigned index;
    int adx;
    int ady;
    unsigned radius;

    if (writer == NULL || motion == NULL ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    /*
     * Family 1 shares its five-bit index space: 0..7 are spatial and 8..31
     * are the radius-three temporal ring. The other families are temporal.
     * Each emitted index is the exact slot the reader's tables enumerate:
     * ring_index and far_index invert ring_vector and far_vector in O(1), so
     * this dispatches by Chebyshev radius rather than scanning every vector.
     */
    if (motion->spatial != 0) {
        const MbMotionVector *table = block_size == MB_MOTION_BLOCK_4X4
                                          ? spatial_4x4
                                          : spatial_2x2;
        for (index = 0U; index < 8U; ++index) {
            if (same_vector(&table[index], motion)) {
                return write_index(writer, 1U, index, 5U);
            }
        }
        return REPLAY_INVALID_ARGUMENT;
    }
    adx = motion->dx < 0 ? -motion->dx : motion->dx;
    ady = motion->dy < 0 ? -motion->dy : motion->dy;
    radius = (unsigned)(adx > ady ? adx : ady);
    if (radius == 1U && ring_index(1U, motion, &index)) {
        return write_index(writer, 0U, index, 3U);
    }
    if (radius == 2U && ring_index(2U, motion, &index)) {
        return write_index(writer, 2U, index, 4U);
    }
    if (radius == 3U && ring_index(3U, motion, &index)) {
        return write_index(writer, 1U, index + 8U, 5U);
    }
    if (radius >= 4U && far_index(motion, &index)) {
        return write_index(writer, 3U, index, 8U);
    }
    return REPLAY_INVALID_ARGUMENT;
}

ReplayStatus mb_motion_read_hq(ReplayBitReader *reader,
                               MbMotionBlockSize block_size,
                               MbMotionVector *motion)
{
    uint32_t family;
    uint32_t index;
    ReplayStatus status;

    if (reader == NULL || motion == NULL ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = replay_bitreader_read(reader, 2U, &family);
    if (status != REPLAY_OK) {
        return status;
    }

    switch (family) {
    case 0U:
        /* Radius one: 2 family bits + 3 index bits. */
        status = replay_bitreader_read(reader, 3U, &index);
        if (status == REPLAY_OK) {
            *motion = ring_vector(1U, index);
        }
        return status;
    case 2U:
        /* Radius two: 2 family bits + 4 index bits. */
        status = replay_bitreader_read(reader, 4U, &index);
        if (status == REPLAY_OK) {
            *motion = ring_vector(2U, index);
        }
        return status;
    case 1U:
        /* Spatial index 0..7, otherwise radius-three index 0..23. */
        status = replay_bitreader_read(reader, 5U, &index);
        if (status != REPLAY_OK) {
            return status;
        }
        if (index < 8U) {
            *motion = block_size == MB_MOTION_BLOCK_4X4
                          ? spatial_4x4[index]
                          : spatial_2x2[index];
        } else {
            *motion = ring_vector(3U, index - 8U);
        }
        return REPLAY_OK;
    case 3U:
        /* Remaining vectors in the [-8,+8] square. */
        status = replay_bitreader_read(reader, 8U, &index);
        return status == REPLAY_OK ? far_vector(index, motion) : status;
    default:
        return REPLAY_INTERNAL_ERROR;
    }
}

ReplayStatus mb_motion_read_format19(ReplayBitReader *reader,
                                     MbMotionBlockSize block_size,
                                     MbMotionVector *motion)
{
    return mb_motion_read_hq(reader, block_size, motion);
}
