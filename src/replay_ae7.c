#include "replay/replay_ae7.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
    unsigned line_number;
} LineReader;

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static ReplayStatus next_line(LineReader *reader, const char **line,
                              size_t *length)
{
    size_t start;

    if (reader->position >= reader->size) {
        return REPLAY_TRUNCATED_INPUT;
    }
    start = reader->position;
    while (reader->position < reader->size &&
           reader->data[reader->position] != (uint8_t)'\n') {
        ++reader->position;
    }
    if (reader->position == reader->size) {
        return REPLAY_TRUNCATED_INPUT;
    }
    *line = (const char *)&reader->data[start];
    *length = reader->position - start;
    if (*length != 0U && (*line)[*length - 1U] == '\r') {
        --*length;
    }
    ++reader->position;
    ++reader->line_number;
    return REPLAY_OK;
}

static ReplayStatus copy_line(LineReader *reader, char *destination,
                              size_t capacity, char *error, size_t error_size)
{
    const char *line;
    size_t length;
    ReplayStatus status = next_line(reader, &line, &length);

    if (status != REPLAY_OK) {
        set_error(error, error_size, "missing header line %u",
                  reader->line_number + 1U);
        return status;
    }
    while (length != 0U && line[length - 1U] == ' ') {
        --length;
    }
    if (length >= capacity) {
        set_error(error, error_size, "header line %u is too long",
                  reader->line_number);
        return REPLAY_MALFORMED_STREAM;
    }
    memcpy(destination, line, length);
    destination[length] = '\0';
    return REPLAY_OK;
}

/* Parse a "NUMBER trailing label" header line. When `label` is non-NULL the
 * text after the number (leading spaces stripped) is copied into it; copy_line
 * has already trimmed trailing spaces. */
static ReplayStatus parse_unsigned_line_label(LineReader *reader,
                                              uint64_t *value, char *label,
                                              size_t label_size, char *error,
                                              size_t error_size)
{
    char line[REPLAY_AE7_TEXT_MAX];
    char *end;
    unsigned long long parsed;
    ReplayStatus status = copy_line(reader, line, sizeof(line),
                                    error, error_size);

    if (status != REPLAY_OK) {
        return status;
    }
    errno = 0;
    parsed = strtoull(line, &end, 10);
    if (end == line || errno == ERANGE) {
        set_error(error, error_size, "invalid integer on header line %u",
                  reader->line_number);
        return REPLAY_MALFORMED_STREAM;
    }
    *value = (uint64_t)parsed;
    if (label != NULL && label_size != 0U) {
        size_t length;
        while (*end == ' ') {
            ++end;
        }
        length = strlen(end);
        if (length >= label_size) {
            length = label_size - 1U;
        }
        memcpy(label, end, length);
        label[length] = '\0';
    }
    return REPLAY_OK;
}


static ReplayStatus parse_signed_line(LineReader *reader, int64_t *value,
                                      char *error, size_t error_size)
{
    char line[REPLAY_AE7_TEXT_MAX];
    char *end;
    long long parsed;
    ReplayStatus status = copy_line(reader, line, sizeof(line),
                                    error, error_size);

    if (status != REPLAY_OK) {
        return status;
    }
    errno = 0;
    parsed = strtoll(line, &end, 10);
    if (end == line || errno == ERANGE) {
        set_error(error, error_size, "invalid integer on header line %u",
                  reader->line_number);
        return REPLAY_MALFORMED_STREAM;
    }
    *value = (int64_t)parsed;
    return REPLAY_OK;
}

static ReplayStatus parse_double_line(LineReader *reader, double *value,
                                      char *error, size_t error_size)
{
    char line[REPLAY_AE7_TEXT_MAX];
    char *end;
    ReplayStatus status = copy_line(reader, line, sizeof(line),
                                    error, error_size);

    if (status != REPLAY_OK) {
        return status;
    }
    errno = 0;
    *value = strtod(line, &end);
    if (end == line || errno == ERANGE) {
        set_error(error, error_size, "invalid number on header line %u",
                  reader->line_number);
        return REPLAY_MALFORMED_STREAM;
    }
    return REPLAY_OK;
}

static ReplayStatus narrow_unsigned(uint64_t value, unsigned *destination,
                                    unsigned line_number,
                                    char *error, size_t error_size)
{
    if (value > UINT_MAX) {
        set_error(error, error_size, "header line %u exceeds unsigned range",
                  line_number);
        return REPLAY_MALFORMED_STREAM;
    }
    *destination = (unsigned)value;
    return REPLAY_OK;
}

static ReplayStatus parse_chunk_line(LineReader *reader, ReplayAe7Chunk *chunk,
                                     char *error, size_t error_size)
{
    char line[REPLAY_AE7_TEXT_MAX];
    char *cursor;
    char *end;
    unsigned long long value;
    ReplayStatus status = copy_line(reader, line, sizeof(line),
                                    error, error_size);

    if (status != REPLAY_OK) {
        return status;
    }
    cursor = line;
    errno = 0;
    value = strtoull(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || *end != ',') {
        goto malformed;
    }
    chunk->file_offset = (uint64_t)value;
    cursor = end + 1;
    errno = 0;
    value = strtoull(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || *end != ';') {
        goto malformed;
    }
    chunk->video_bytes = (uint64_t)value;
    cursor = end;
    while (*cursor == ';') {
        ++cursor;
        errno = 0;
        value = strtoull(cursor, &end, 10);
        if (end == cursor || errno == ERANGE ||
            chunk->sound_bytes > UINT64_MAX - (uint64_t)value) {
            goto malformed;
        }
        chunk->sound_bytes += (uint64_t)value;
        ++chunk->sound_tracks;
        cursor = end;
    }
    if (*cursor != '\0') {
        goto malformed;
    }
    return REPLAY_OK;

malformed:
    set_error(error, error_size, "invalid catalogue entry on line %u",
              reader->line_number);
    return REPLAY_MALFORMED_STREAM;
}

static ReplayStatus validate_chunks(const ReplayAe7Movie *movie, size_t size,
                                    char *error, size_t error_size)
{
    size_t index;

    for (index = 0U; index < movie->chunk_count; ++index) {
        const ReplayAe7Chunk *chunk = &movie->chunks[index];
        uint64_t payload_bytes;

        if (chunk->video_bytes > UINT64_MAX - chunk->sound_bytes) {
            set_error(error, error_size, "chunk %zu payload size overflows",
                      index);
            return REPLAY_MALFORMED_STREAM;
        }
        payload_bytes = chunk->video_bytes + chunk->sound_bytes;
        if (chunk->file_offset > (uint64_t)size ||
            payload_bytes > (uint64_t)size - chunk->file_offset) {
            set_error(error, error_size, "chunk %zu lies outside the file",
                      index);
            return REPLAY_TRUNCATED_INPUT;
        }
    }
    return REPLAY_OK;
}

ReplayStatus replay_ae7_parse(const uint8_t *data, size_t size,
                              ReplayAe7Movie *movie,
                              char *error, size_t error_size)
{
    LineReader reader = { data, size, 0U, 0U };
    char magic[16];
    uint64_t fields[15];
    char labels[15][REPLAY_AE7_TEXT_MAX];
    size_t index;
    ReplayStatus status;

    if (data == NULL || movie == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    memset(movie, 0, sizeof(*movie));
    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    status = copy_line(&reader, magic, sizeof(magic), error, error_size);
    if (status != REPLAY_OK) {
        return status;
    }
    if (strcmp(magic, "ARMovie") != 0) {
        set_error(error, error_size, "not an ARMovie file");
        return REPLAY_MALFORMED_STREAM;
    }
    if ((status = copy_line(&reader, movie->title, sizeof(movie->title),
                            error, error_size)) != REPLAY_OK ||
        (status = copy_line(&reader, movie->copyright,
                            sizeof(movie->copyright), error,
                            error_size)) != REPLAY_OK ||
        (status = copy_line(&reader, movie->author, sizeof(movie->author),
                            error, error_size)) != REPLAY_OK) {
        return status;
    }

    for (index = 0U; index < 4U; ++index) {
        if ((status = parse_unsigned_line_label(
                 &reader, &fields[index], labels[index], sizeof(labels[index]),
                 error, error_size)) != REPLAY_OK) {
            return status;
        }
    }
    if ((status = parse_double_line(&reader, &movie->frames_per_second,
                                    error, error_size)) != REPLAY_OK) {
        return status;
    }
    for (index = 4U; index < 15U; ++index) {
        if ((status = parse_unsigned_line_label(
                 &reader, &fields[index], labels[index], sizeof(labels[index]),
                 error, error_size)) != REPLAY_OK) {
            return status;
        }
    }
    if ((status = parse_signed_line(&reader, &movie->key_frame_offset,
                                    error, error_size)) != REPLAY_OK) {
        return status;
    }

#define NARROW(field, source, line)                                           \
    do {                                                                      \
        status = narrow_unsigned((source), &(field), (line),                  \
                                 error, error_size);                          \
        if (status != REPLAY_OK) {                                            \
            return status;                                                    \
        }                                                                     \
    } while (0)
    NARROW(movie->video_codec, fields[0], 5U);
    NARROW(movie->width, fields[1], 6U);
    NARROW(movie->height, fields[2], 7U);
    NARROW(movie->pixel_depth, fields[3], 8U);
    NARROW(movie->sound_codec, fields[4], 10U);
    NARROW(movie->sound_rate, fields[5], 11U);
    NARROW(movie->sound_channels, fields[6], 12U);
    NARROW(movie->sound_precision, fields[7], 13U);
    NARROW(movie->frames_per_chunk, fields[8], 14U);
    NARROW(movie->last_chunk, fields[9], 15U);
#undef NARROW
    /* Trailing labels carry the video colour model and the sound sub-format. */
    memcpy(movie->video_label, labels[0], sizeof(movie->video_label));
    memcpy(movie->pixel_label, labels[3], sizeof(movie->pixel_label));
    memcpy(movie->sound_format_label, labels[4],
           sizeof(movie->sound_format_label));
    memcpy(movie->sound_channels_label, labels[6],
           sizeof(movie->sound_channels_label));
    memcpy(movie->sound_precision_label, labels[7],
           sizeof(movie->sound_precision_label));
    movie->even_chunk_bytes = fields[10];
    movie->odd_chunk_bytes = fields[11];
    movie->catalogue_offset = fields[12];
    movie->sprite_offset = fields[13];
    movie->sprite_bytes = fields[14];

    /* A sound-only movie (video format 0) legitimately has zero dimensions. */
    if ((movie->video_codec != 0U &&
         (movie->width == 0U || movie->height == 0U)) ||
        movie->frames_per_chunk == 0U ||
        !isfinite(movie->frames_per_second) ||
        movie->frames_per_second <= 0.0) {
        set_error(error, error_size, "invalid video dimensions or timing");
        return REPLAY_MALFORMED_STREAM;
    }
    if (movie->catalogue_offset > (uint64_t)size) {
        set_error(error, error_size, "catalogue offset lies outside the file");
        return REPLAY_TRUNCATED_INPUT;
    }
    if (movie->sprite_offset > (uint64_t)size ||
        movie->sprite_bytes > (uint64_t)size - movie->sprite_offset) {
        set_error(error, error_size, "sprite lies outside the file");
        return REPLAY_TRUNCATED_INPUT;
    }
    if (movie->key_frame_offset < -1 ||
        (movie->key_frame_offset >= 0 &&
         (uint64_t)movie->key_frame_offset > (uint64_t)size)) {
        set_error(error, error_size, "key-frame offset lies outside the file");
        return REPLAY_TRUNCATED_INPUT;
    }
    if (movie->last_chunk == UINT_MAX ||
        (uint64_t)movie->last_chunk + 1U > SIZE_MAX / sizeof(*movie->chunks)) {
        set_error(error, error_size, "chunk count is too large");
        return REPLAY_MALFORMED_STREAM;
    }
    movie->chunk_count = (size_t)movie->last_chunk + 1U;
    /* Even the shortest legal catalogue line, "0,0;0\n", is six bytes. */
    if (movie->chunk_count >
        (size - (size_t)movie->catalogue_offset) / 6U) {
        set_error(error, error_size, "catalogue cannot contain %zu entries",
                  movie->chunk_count);
        return REPLAY_TRUNCATED_INPUT;
    }
    movie->chunks = calloc(movie->chunk_count, sizeof(*movie->chunks));
    if (movie->chunks == NULL) {
        return REPLAY_OUT_OF_MEMORY;
    }
    reader.position = (size_t)movie->catalogue_offset;
    reader.line_number = 0U;
    for (index = 0U; index < movie->chunk_count; ++index) {
        status = parse_chunk_line(&reader, &movie->chunks[index],
                                  error, error_size);
        if (status != REPLAY_OK) {
            goto fail;
        }
    }
    status = validate_chunks(movie, size, error, error_size);
    if (status != REPLAY_OK) {
        goto fail;
    }
    return REPLAY_OK;

fail:
    replay_ae7_movie_destroy(movie);
    return status;
}

void replay_ae7_movie_destroy(ReplayAe7Movie *movie)
{
    if (movie != NULL) {
        free(movie->chunks);
        movie->chunks = NULL;
        movie->chunk_count = 0U;
    }
}
