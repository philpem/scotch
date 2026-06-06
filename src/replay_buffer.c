#include "replay/replay_buffer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void replay_buffer_init(ReplayBuffer *buffer)
{
    if (buffer == NULL) {
        return;
    }
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

ReplayStatus replay_buffer_reserve(ReplayBuffer *buffer, size_t capacity)
{
    uint8_t *new_data;
    size_t new_capacity;

    if (buffer == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (capacity <= buffer->capacity) {
        return REPLAY_OK;
    }

    new_capacity = buffer->capacity == 0 ? 64U : buffer->capacity;
    while (new_capacity < capacity) {
        if (new_capacity > SIZE_MAX / 2U) {
            new_capacity = capacity;
            break;
        }
        new_capacity *= 2U;
    }

    new_data = realloc(buffer->data, new_capacity);
    if (new_data == NULL) {
        return REPLAY_OUT_OF_MEMORY;
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return REPLAY_OK;
}

ReplayStatus replay_buffer_append_u8(ReplayBuffer *buffer, uint8_t value)
{
    ReplayStatus status;

    if (buffer == NULL || buffer->size == SIZE_MAX) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = replay_buffer_reserve(buffer, buffer->size + 1U);
    if (status != REPLAY_OK) {
        return status;
    }
    buffer->data[buffer->size++] = value;
    return REPLAY_OK;
}

ReplayStatus replay_buffer_append(ReplayBuffer *buffer, const void *data,
                                  size_t size)
{
    ReplayStatus status;

    if (buffer == NULL || (data == NULL && size != 0U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    if (size > SIZE_MAX - buffer->size) {
        return REPLAY_INVALID_ARGUMENT;
    }
    status = replay_buffer_reserve(buffer, buffer->size + size);
    if (status != REPLAY_OK) {
        return status;
    }
    if (size != 0U) {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
    }
    return REPLAY_OK;
}

void replay_buffer_truncate(ReplayBuffer *buffer, size_t size)
{
    if (buffer != NULL && size < buffer->size) {
        buffer->size = size;
    }
}

void replay_buffer_clear(ReplayBuffer *buffer)
{
    if (buffer != NULL) {
        buffer->size = 0;
    }
}

void replay_buffer_free(ReplayBuffer *buffer)
{
    if (buffer == NULL) {
        return;
    }
    free(buffer->data);
    replay_buffer_init(buffer);
}

