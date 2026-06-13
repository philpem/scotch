#include "replay/replay_ae7_write.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * See notes/replay-ae7-join-writer.md for the authoritative layout this writer
 * reproduces. The header is 21 newline-terminated text lines; the three offset
 * fields are forward references resolved by a short fixed-point iteration. Each
 * chunk's file offset is rounded up to a sector boundary, its video region is
 * padded to an even length, and its sound tracks follow the video.
 */

#define REPLAY_AE7_MAX_TRACKS 8U

typedef struct {
    size_t first_frame;
    size_t frame_count;
    uint64_t video_bytes;                          /* padded to even */
    uint64_t sound_offset[REPLAY_AE7_MAX_TRACKS];  /* byte offset into track */
    uint64_t sound_bytes[REPLAY_AE7_MAX_TRACKS];
    uint64_t sound_total;
    uint64_t file_offset;
} ChunkInfo;

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static ReplayStatus append_fmt(ReplayBuffer *buffer, const char *fmt, ...)
{
    char line[256];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written < 0) {
        return REPLAY_INTERNAL_ERROR;
    }
    if ((size_t)written >= sizeof(line)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    return replay_buffer_append(buffer, line, (size_t)written);
}

static uint64_t align_up(uint64_t value, unsigned mask)
{
    return (value + (uint64_t)mask) & ~(uint64_t)mask;
}

static const char *text_or_empty(const char *text)
{
    return text != NULL ? text : "";
}

/* Bytes per sample frame for time-based slicing of a continuous track. */
static uint64_t track_frame_bytes(const ReplayAe7WriteTrack *track)
{
    unsigned bytes_per_sample = (track->precision_bits + 7U) / 8U;

    if (bytes_per_sample == 0U) {
        bytes_per_sample = 1U;
    }
    return (uint64_t)track->channels * (uint64_t)bytes_per_sample;
}

/*
 * Render the 21-line header for the given offsets. Returns the byte length in
 * *header_len. The buffer is cleared first.
 */
static ReplayStatus render_header(const ReplayAe7WriteOptions *options,
                                  uint64_t even_size, uint64_t odd_size,
                                  double frames_per_chunk_nominal,
                                  size_t chunk_count,
                                  uint64_t catalogue_offset,
                                  uint64_t sprite_offset, uint64_t sprite_size,
                                  int64_t keys_offset,
                                  ReplayBuffer *header)
{
    const ReplayAe7WriteTrack *track =
        options->track_count != 0U ? &options->tracks[0] : NULL;
    unsigned sound_codec = track != NULL ? track->codec : REPLAY_AE7_SOUND_NONE;
    unsigned sound_rate = track != NULL ? track->rate_hz : 0U;
    unsigned sound_channels = track != NULL ? track->channels : 0U;
    unsigned sound_precision = track != NULL ? track->precision_bits : 0U;
    const char *sound_label =
        track != NULL && track->label != NULL
            ? track->label
            : (sound_codec == REPLAY_AE7_SOUND_VIDC_LOG
                   ? "bits per sample (exponential)"
                   : "bits per sample");
    ReplayStatus status;

    replay_buffer_clear(header);

    status = append_fmt(header, "ARMovie\n");
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%s\n", text_or_empty(options->title));
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%s\n", text_or_empty(options->copyright));
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%s\n", text_or_empty(options->author));
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%u video format\n", options->video_codec);
    }
    if (status == REPLAY_OK) {
        /* A sound-only movie (format 0) must report zero dimensions: the player
         * crashes if it sees a non-zero frame size with no video decompressor. */
        status = append_fmt(header, "%u pixels\n",
                            options->video_codec != 0U ? options->width : 0U);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%u pixels\n",
                            options->video_codec != 0U ? options->height : 0U);
    }
    if (status == REPLAY_OK) {
        if (options->pixel_label != NULL) {
            status = append_fmt(header, "%u bits per pixel [%s]\n",
                                options->pixel_depth, options->pixel_label);
        } else {
            status = append_fmt(header, "%u bits per pixel\n",
                                options->pixel_depth);
        }
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%g frames per second\n",
                            options->frames_per_second);
    }
    if (status == REPLAY_OK) {
        /* Format 2 names its decompressor inline: "2 <name>". The player reads
         * the token after the number, so other formats keep an annotation. */
        if (sound_codec == REPLAY_AE7_SOUND_NAMED && track != NULL &&
            track->codec_name != NULL) {
            status = append_fmt(header, "%u %s\n", sound_codec,
                                track->codec_name);
        } else {
            status = append_fmt(header, "%u sound format\n", sound_codec);
        }
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%u Hz samples\n", sound_rate);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%u channels\n", sound_channels);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%u %s\n", sound_precision, sound_label);
    }
    if (status == REPLAY_OK) {
        /* Join reads this with atof and accepts a decimal point, so a
         * fractional rate such as "12.5 frames per chunk" is legal and is
         * emitted verbatim rather than rounded. */
        status = append_fmt(header, "%g frames per chunk\n",
                            frames_per_chunk_nominal);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%zu number of chunks\n", chunk_count - 1U);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%llu even chunk size\n",
                            (unsigned long long)even_size);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%llu odd chunk size\n",
                            (unsigned long long)odd_size);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%llu catalogue offset\n",
                            (unsigned long long)catalogue_offset);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%llu offset to sprite\n",
                            (unsigned long long)sprite_offset);
    }
    if (status == REPLAY_OK) {
        status = append_fmt(header, "%llu size of sprite\n",
                            (unsigned long long)sprite_size);
    }
    if (status == REPLAY_OK) {
        if (keys_offset >= 0) {
            status = append_fmt(header, "%lld offset to keys\n",
                                (long long)keys_offset);
        } else {
            status = append_fmt(header, "-1 (no keys)\n");
        }
    }
    return status;
}

/* Append the catalogue text for the current chunk offsets into `catalogue`,
 * assigning each chunk's file_offset starting at `first_data`. */
static ReplayStatus build_catalogue(ChunkInfo *chunks, size_t chunk_count,
                                    size_t track_count, uint64_t first_data,
                                    unsigned align_mask, ReplayBuffer *catalogue)
{
    uint64_t offset = first_data;
    size_t index;
    ReplayStatus status = REPLAY_OK;

    replay_buffer_clear(catalogue);
    for (index = 0U; index < chunk_count && status == REPLAY_OK; ++index) {
        ChunkInfo *chunk = &chunks[index];
        size_t track;

        chunk->file_offset = offset;
        status = append_fmt(catalogue, "%llu,%llu",
                            (unsigned long long)offset,
                            (unsigned long long)chunk->video_bytes);
        for (track = 0U; track < track_count && status == REPLAY_OK; ++track) {
            status = append_fmt(catalogue, ";%llu",
                                (unsigned long long)chunk->sound_bytes[track]);
        }
        /* The catalogue always carries a sound field; a movie with no sound
         * uses a single ";0", matching Join and the parser's expectation. */
        if (status == REPLAY_OK && track_count == 0U) {
            status = append_fmt(catalogue, ";0");
        }
        if (status == REPLAY_OK) {
            status = append_fmt(catalogue, "\n");
        }
        offset = align_up(offset + chunk->video_bytes + chunk->sound_total,
                          align_mask);
    }
    return status;
}

static ReplayStatus append_zeros(ReplayBuffer *buffer, uint64_t count)
{
    static const uint8_t zeros[256] = { 0 };

    while (count != 0U) {
        size_t step = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
        ReplayStatus status = replay_buffer_append(buffer, zeros, step);

        if (status != REPLAY_OK) {
            return status;
        }
        count -= (uint64_t)step;
    }
    return REPLAY_OK;
}

ReplayStatus replay_ae7_write(const ReplayAe7WriteOptions *options,
                              ReplayBuffer *out,
                              char *error, size_t error_size)
{
    ChunkInfo *chunks = NULL;
    ReplayBuffer header;
    ReplayBuffer catalogue;
    size_t chunk_count;
    size_t index;
    size_t track;
    int sound_only;
    double chunk_secs;
    double total_seconds = 0.0;
    double frames_target = 1.0;
    double frames_per_chunk_nominal;
    unsigned align_mask;
    uint64_t even_size = 0U;
    uint64_t odd_size = 0U;
    uint64_t sprite_size = (uint64_t)options->sprite_size;
    uint64_t keys_total;
    size_t key_count;
    uint64_t header_len = 0U;
    uint64_t catalogue_offset = 0U;
    uint64_t sprite_offset = 0U;
    int64_t keys_offset;
    uint64_t first_data;
    int iteration;
    ReplayStatus status = REPLAY_OK;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (options == NULL || out == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    sound_only = options->frame_count == 0U;
    if (sound_only) {
        if (options->track_count == 0U) {
            set_error(error, error_size,
                      "a sound-only movie needs at least one sound track");
            return REPLAY_INVALID_ARGUMENT;
        }
    } else if (options->frame_data == NULL || options->frame_size == NULL) {
        set_error(error, error_size, "no video frames supplied");
        return REPLAY_INVALID_ARGUMENT;
    }
    if (!(options->frames_per_second > 0.0)) {
        set_error(error, error_size, "frame rate must be positive");
        return REPLAY_INVALID_ARGUMENT;
    }
    if (options->track_count > REPLAY_AE7_MAX_TRACKS) {
        set_error(error, error_size, "too many sound tracks");
        return REPLAY_INVALID_ARGUMENT;
    }
    /* Per-track requirements. The format number itself is not capped: the
     * writer carries pre-encoded bytes, and format 2 names its decompressor. */
    for (track = 0U; track < options->track_count; ++track) {
        const ReplayAe7WriteTrack *t = &options->tracks[track];

        if (t->codec == REPLAY_AE7_SOUND_NONE) {
            set_error(error, error_size, "a sound track needs a non-zero format");
            return REPLAY_INVALID_ARGUMENT;
        }
        if (t->codec == REPLAY_AE7_SOUND_NAMED &&
            (t->codec_name == NULL || t->codec_name[0] == '\0')) {
            set_error(error, error_size,
                      "sound format 2 requires a decompressor name");
            return REPLAY_INVALID_ARGUMENT;
        }
        if (t->channels != 1U && t->channels != 2U) {
            set_error(error, error_size, "sound channels must be 1 or 2");
            return REPLAY_INVALID_ARGUMENT;
        }
        if (t->rate_hz == 0U) {
            set_error(error, error_size, "sound rate must be positive");
            return REPLAY_INVALID_ARGUMENT;
        }
    }
    if (options->write_keys &&
        (options->key_data == NULL || options->key_size == 0U)) {
        set_error(error, error_size, "key frames requested but not supplied");
        return REPLAY_INVALID_ARGUMENT;
    }

    align_mask = options->align_mask != 0U ? options->align_mask
                                           : REPLAY_AE7_DEFAULT_ALIGN_MASK;
    /* The mask must be one less than a power of two for the rounding to work. */
    if ((align_mask & (align_mask + 1U)) != 0U) {
        set_error(error, error_size, "alignment mask must be 2^n - 1");
        return REPLAY_INVALID_ARGUMENT;
    }

    /* Chunk duration in seconds: an explicit frame count divided by the rate,
     * otherwise the target chunk length (default one second). */
    if (options->frames_per_chunk != 0U) {
        chunk_secs = (double)options->frames_per_chunk /
                     options->frames_per_second;
    } else {
        chunk_secs = options->chunk_seconds > 0.0 ? options->chunk_seconds : 1.0;
    }
    frames_target = options->frames_per_second * chunk_secs;
    if (!sound_only && frames_target < 1.0) {
        frames_target = 1.0;
        chunk_secs = frames_target / options->frames_per_second;
    }
    /*
     * The player double-buffers chunks and aliases its two buffers (op1 = op0)
     * when a movie has only one chunk. With sound that is fatal: the look-ahead
     * prefetch of the (non-existent) next chunk overwrites the chunk still being
     * played, corrupting video a few frames in and losing sound. Force at least
     * two chunks whenever there is audio and there are frames enough to split.
     */
    if (!sound_only && options->track_count != 0U &&
        options->frame_count >= 2U &&
        frames_target >= (double)options->frame_count) {
        frames_target = (double)options->frame_count / 2.0;
        chunk_secs = frames_target / options->frames_per_second;
    }
    /* Emit the nominal rate faithfully: an exact integer when the caller asked
     * for a fixed frame count, otherwise the fractional target (e.g. 12.5). */
    frames_per_chunk_nominal = frames_target;

    if (sound_only) {
        /* Chunk count covers the longest track at the chosen chunk duration. */
        for (track = 0U; track < options->track_count; ++track) {
            const ReplayAe7WriteTrack *t = &options->tracks[track];
            double frame_bytes = (double)track_frame_bytes(t);
            double seconds = frame_bytes > 0.0 && t->rate_hz != 0U
                                 ? (double)t->size / (frame_bytes *
                                                      (double)t->rate_hz)
                                 : 0.0;

            if (seconds > total_seconds) {
                total_seconds = seconds;
            }
        }
        chunk_count = (size_t)ceil(total_seconds / chunk_secs);
        if (chunk_count == 0U) {
            chunk_count = 1U;
        }
    } else {
        /* Partition frames into chunks with floor((i+1)*F) - floor(i*F). */
        size_t assigned = 0U;
        size_t i = 0U;

        chunk_count = 0U;
        while (assigned < options->frame_count) {
            double next = floor((double)(i + 1U) * frames_target);
            size_t end = (size_t)next;

            if (end <= assigned) {
                end = assigned + 1U;
            }
            if (end > options->frame_count) {
                end = options->frame_count;
            }
            assigned = end;
            ++i;
            ++chunk_count;
        }
    }

    chunks = calloc(chunk_count, sizeof(*chunks));
    if (chunks == NULL) {
        return REPLAY_OUT_OF_MEMORY;
    }
    replay_buffer_init(&header);
    replay_buffer_init(&catalogue);

    /* Fill chunk frame ranges, video sizes, and time-sliced sound sizes. The
     * chunk's [t0, t1) second span drives the sound slice for both paths. */
    {
        size_t assigned = 0U;
        for (index = 0U; index < chunk_count; ++index) {
            ChunkInfo *chunk = &chunks[index];
            double t0;
            double t1;

            if (sound_only) {
                t0 = (double)index * chunk_secs;
                t1 = (double)(index + 1U) * chunk_secs;
                if (t1 > total_seconds) {
                    t1 = total_seconds;
                }
                chunk->first_frame = 0U;
                chunk->frame_count = 0U;
                chunk->video_bytes = 0U;
            } else {
                double next = floor((double)(index + 1U) * frames_target);
                size_t end = (size_t)next;
                uint64_t video = 0U;
                size_t f;

                if (end <= assigned) {
                    end = assigned + 1U;
                }
                if (end > options->frame_count) {
                    end = options->frame_count;
                }
                chunk->first_frame = assigned;
                chunk->frame_count = end - assigned;
                for (f = assigned; f < end; ++f) {
                    video += (uint64_t)options->frame_size[f];
                }
                if ((video & 1U) != 0U) {
                    ++video; /* even (halfword) alignment */
                }
                chunk->video_bytes = video;
                t0 = (double)assigned / options->frames_per_second;
                t1 = (double)end / options->frames_per_second;
                assigned = end;
            }

            for (track = 0U; track < options->track_count; ++track) {
                const ReplayAe7WriteTrack *t = &options->tracks[track];
                uint64_t frame_bytes = track_frame_bytes(t);
                double rate = (double)t->rate_hz;
                uint64_t s0 = (uint64_t)(t0 * rate + 0.5);
                uint64_t s1 = (uint64_t)(t1 * rate + 0.5);

                chunk->sound_offset[track] = s0 * frame_bytes;
                chunk->sound_bytes[track] = (s1 - s0) * frame_bytes;
                chunk->sound_total += chunk->sound_bytes[track];
            }
        }
    }

    /*
     * The player sizes its single sound buffer from the header's frames-per-
     * chunk, so that value must cover the chunk with the most frames. With the
     * fractional distribution (e.g. 12/13 for 12.5) the average would undersize
     * the buffer for the longer chunks and corrupt playback, so report the
     * maximum when there is sound. Video-only movies keep the exact average.
     */
    if (options->track_count != 0U && !sound_only) {
        size_t max_frames = 0U;

        for (index = 0U; index < chunk_count; ++index) {
            if (chunks[index].frame_count > max_frames) {
                max_frames = chunks[index].frame_count;
            }
        }
        if ((double)max_frames > frames_per_chunk_nominal) {
            frames_per_chunk_nominal = (double)max_frames;
        }
    }

    /* even/odd chunk size = max(video + sound) over that parity + 1 guard. */
    for (index = 0U; index < chunk_count; ++index) {
        uint64_t total = chunks[index].video_bytes + chunks[index].sound_total;

        if ((index & 1U) == 0U) {
            if (total > even_size) {
                even_size = total;
            }
        } else {
            if (total > odd_size) {
                odd_size = total;
            }
        }
    }
    if (even_size != 0U) {
        even_size += 1U;
    }
    if (odd_size != 0U) {
        odd_size += 1U;
    }

    /*
     * The key-frame area holds one block per chunk except the first (the player
     * reads block fchunk-1 to start chunk fchunk, and chunk 0 starts unaided):
     * chunk_count-1 blocks of key_size bytes. key_data[j] is the start state for
     * chunk j+1 (the reconstruction at the end of chunk j).
     */
    key_count = (options->write_keys && chunk_count >= 2U) ? chunk_count - 1U
                                                           : 0U;
    keys_total = (uint64_t)key_count * (uint64_t)options->key_size;

    /* Fixed-point on the header length: the three offset fields depend on it. */
    for (iteration = 0; iteration < 8; ++iteration) {
        /* With no sprite the offset field is 0 (as in Acorn's own movies); the
         * sprite, when present, sits immediately after the text header. */
        sprite_offset = sprite_size != 0U ? header_len : 0U;
        catalogue_offset = header_len + sprite_size + keys_total;
        keys_offset = key_count != 0U
                          ? (int64_t)(header_len + sprite_size)
                          : -1;
        status = render_header(options, even_size, odd_size,
                               frames_per_chunk_nominal, chunk_count,
                               catalogue_offset, sprite_offset, sprite_size,
                               keys_offset, &header);
        if (status != REPLAY_OK) {
            goto done;
        }
        if ((uint64_t)header.size == header_len) {
            break;
        }
        header_len = (uint64_t)header.size;
    }

    /* Converge the first chunk offset so the catalogue fits before it. */
    first_data = align_up(catalogue_offset + (uint64_t)chunk_count * 30U,
                          align_mask);
    for (iteration = 0; iteration < 64; ++iteration) {
        status = build_catalogue(chunks, chunk_count, options->track_count,
                                 first_data, align_mask, &catalogue);
        if (status != REPLAY_OK) {
            goto done;
        }
        if (catalogue_offset + (uint64_t)catalogue.size <= first_data) {
            break;
        }
        first_data += (uint64_t)align_mask + 1U;
    }

    /* Assemble the file: header, sprite, keys, catalogue, aligned chunks. */
    replay_buffer_clear(out);
    status = replay_buffer_append(out, header.data, header.size);
    if (status == REPLAY_OK && sprite_size != 0U) {
        status = replay_buffer_append(out, options->sprite_data,
                                      options->sprite_size);
    }
    for (index = 0U; index < key_count && status == REPLAY_OK; ++index) {
        /* The key to start chunk index+1 is the reconstruction of the last frame
         * of chunk index; key_data is indexed by video frame. */
        size_t boundary =
            chunks[index].first_frame + chunks[index].frame_count - 1U;

        status = replay_buffer_append(out, options->key_data[boundary],
                                      options->key_size);
    }
    if (status == REPLAY_OK) {
        status = replay_buffer_append(out, catalogue.data, catalogue.size);
    }

    for (index = 0U; index < chunk_count && status == REPLAY_OK; ++index) {
        ChunkInfo *chunk = &chunks[index];
        uint64_t video_written = 0U;
        size_t f;

        status = append_zeros(out, chunk->file_offset - (uint64_t)out->size);
        for (f = chunk->first_frame;
             f < chunk->first_frame + chunk->frame_count && status == REPLAY_OK;
             ++f) {
            status = replay_buffer_append(out, options->frame_data[f],
                                          options->frame_size[f]);
            video_written += (uint64_t)options->frame_size[f];
        }
        if (status == REPLAY_OK && video_written < chunk->video_bytes) {
            status = append_zeros(out, chunk->video_bytes - video_written);
        }
        for (track = 0U; track < options->track_count && status == REPLAY_OK;
             ++track) {
            const ReplayAe7WriteTrack *t = &options->tracks[track];
            uint64_t want = chunk->sound_bytes[track];
            uint64_t start = chunk->sound_offset[track];
            uint64_t avail = start < (uint64_t)t->size
                                 ? (uint64_t)t->size - start
                                 : 0U;
            uint64_t copy = want < avail ? want : avail;

            if (copy != 0U) {
                status = replay_buffer_append(out, t->data + start,
                                              (size_t)copy);
            }
            if (status == REPLAY_OK && copy < want) {
                status = append_zeros(out, want - copy); /* trailing silence */
            }
        }
    }

done:
    if (status != REPLAY_OK && error != NULL && error_size != 0U &&
        error[0] == '\0') {
        set_error(error, error_size, replay_status_string(status));
    }
    replay_buffer_free(&header);
    replay_buffer_free(&catalogue);
    free(chunks);
    return status;
}
