#include "replay/replay_status.h"

const char *replay_status_string(ReplayStatus status)
{
    switch (status) {
    case REPLAY_OK:
        return "ok";
    case REPLAY_INVALID_ARGUMENT:
        return "invalid argument";
    case REPLAY_OUT_OF_MEMORY:
        return "out of memory";
    case REPLAY_TRUNCATED_INPUT:
        return "truncated input";
    case REPLAY_MALFORMED_STREAM:
        return "malformed stream";
    case REPLAY_UNSUPPORTED_CODEC:
        return "unsupported codec";
    case REPLAY_INTERNAL_ERROR:
        return "internal error";
    }
    return "unknown status";
}

