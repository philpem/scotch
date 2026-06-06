#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_status.h"

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ReplayBuffer;

void replay_buffer_init(ReplayBuffer *buffer);
ReplayStatus replay_buffer_reserve(ReplayBuffer *buffer, size_t capacity);
ReplayStatus replay_buffer_append_u8(ReplayBuffer *buffer, uint8_t value);
ReplayStatus replay_buffer_append(ReplayBuffer *buffer, const void *data,
                                  size_t size);
void replay_buffer_truncate(ReplayBuffer *buffer, size_t size);
void replay_buffer_clear(ReplayBuffer *buffer);
void replay_buffer_free(ReplayBuffer *buffer);

#endif

