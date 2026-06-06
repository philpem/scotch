#include "replay/mb_motion.h"

#include <stddef.h>

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

    if (writer == NULL || motion == NULL ||
        (block_size != MB_MOTION_BLOCK_2X2 &&
         block_size != MB_MOTION_BLOCK_4X4)) {
        return REPLAY_INVALID_ARGUMENT;
    }
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
    for (index = 0U; index < 8U; ++index) {
        MbMotionVector candidate = ring_vector(1U, index);
        if (same_vector(&candidate, motion)) {
            return write_index(writer, 0U, index, 3U);
        }
    }
    for (index = 0U; index < 16U; ++index) {
        MbMotionVector candidate = ring_vector(2U, index);
        if (same_vector(&candidate, motion)) {
            return write_index(writer, 2U, index, 4U);
        }
    }
    for (index = 0U; index < 24U; ++index) {
        MbMotionVector candidate = ring_vector(3U, index);
        if (same_vector(&candidate, motion)) {
            return write_index(writer, 1U, index + 8U, 5U);
        }
    }
    for (index = 0U; index < 240U; ++index) {
        MbMotionVector candidate;
        if (far_vector(index, &candidate) == REPLAY_OK &&
            same_vector(&candidate, motion)) {
            return write_index(writer, 3U, index, 8U);
        }
    }
    return REPLAY_INVALID_ARGUMENT;
}

ReplayStatus mb_motion_read_format19(ReplayBitReader *reader,
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
        status = replay_bitreader_read(reader, 3U, &index);
        if (status == REPLAY_OK) {
            *motion = ring_vector(1U, index);
        }
        return status;
    case 2U:
        status = replay_bitreader_read(reader, 4U, &index);
        if (status == REPLAY_OK) {
            *motion = ring_vector(2U, index);
        }
        return status;
    case 1U:
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
        status = replay_bitreader_read(reader, 8U, &index);
        return status == REPLAY_OK ? far_vector(index, motion) : status;
    default:
        return REPLAY_INTERNAL_ERROR;
    }
}
