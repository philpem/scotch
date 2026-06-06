#ifndef REPLAY_STATUS_H
#define REPLAY_STATUS_H

typedef enum {
    REPLAY_OK = 0,
    REPLAY_INVALID_ARGUMENT,
    REPLAY_OUT_OF_MEMORY,
    REPLAY_TRUNCATED_INPUT,
    REPLAY_MALFORMED_STREAM,
    REPLAY_UNSUPPORTED_CODEC,
    REPLAY_INTERNAL_ERROR
} ReplayStatus;

const char *replay_status_string(ReplayStatus status);

#endif

