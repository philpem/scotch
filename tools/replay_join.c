#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_sound.h"

#include "default_poster.h"

/*
 * replay-join assembles raw per-frame codec payloads (as emitted by
 * `replay-encode --payload-prefix`) into a playable Acorn Replay/AE7 movie,
 * mirroring Acorn's own `Join` tool. Frame payloads are grouped into chunks and
 * an optional sound track is interleaved per chunk. See
 * notes/replay-ae7-join-writer.md.
 */

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-join --codec N --size WxH --fps F "
            "--frames-prefix PREFIX --frames N --output FILE\n"
            "  [--frame-suffix .mb19] [--pixel-depth 16] [--pixel-label LABEL]\n"
            "  [--frames-per-chunk N | --chunk-seconds S] [--align MASK]\n"
            "  [--title T] [--copyright C] [--author A]\n"
            "  [--keys-prefix PREFIX]   (per-frame .key files for chunk seeking)\n"
            "  [--poster FILE.bgr555 [--poster-size WxH]] [--no-poster]\n"
            "    (16bpp poster for !ARPlayer; default = built-in Replay logo)\n"
            "  --sound-rate HZ --sound-channels N]\n"
            "  encoded audio:  --sound-pcm FILE.s16le\n"
            "                  --sound-encode vidc-e8|signed-8|signed-16\n"
            "  pre-encoded:    --sound FILE --sound-format vidc-log|adpcm|gsm|..."
            "\n                  (vidc-log = format 1; other = format 2"
            " \"2 <name>\")\n");
}

static int read_file(const char *path, ReplayBuffer *buffer)
{
    uint8_t block[65536];
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(block, 1U, sizeof(block), file);

        if (count != 0U &&
            replay_buffer_append(buffer, block, count) != REPLAY_OK) {
            fprintf(stderr, "%s: unable to allocate input buffer\n", path);
            fclose(file);
            return EXIT_FAILURE;
        }
        if (count != sizeof(block)) {
            if (ferror(file)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
            break;
        }
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static void put_u32(ReplayBuffer *b, uint32_t v)
{
    uint8_t bytes[4] = {
        (uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU),
        (uint8_t)((v >> 16) & 0xFFU), (uint8_t)((v >> 24) & 0xFFU)
    };

    (void)replay_buffer_append(b, bytes, sizeof(bytes));
}

/*
 * Wrap raw 16bpp (bgr555, R in the low bits) pixels into a complete RISC OS
 * spritefile (12-byte area header + 44-byte sprite control block + image), the
 * "helpful sprite" poster !ARPlayer displays. Rows are padded to a word.
 *
 * The mode field is a new-format sprite mode word selecting 16bpp (type 5,
 * 1:5:5:5) at 90x90 dpi, i.e. square pixels: (5<<27)|(90<<14)|(90<<1)|1. Old
 * numbered modes such as 28 are 8bpp and would render the 16bpp data as garbage
 * 256-colour pixels, and square pixels mean the poster is stored at the movie's
 * true dimensions (no 2:1 height doubling).
 */
#define POSTER_SPRITE_MODE ((5u << 27) | (90u << 14) | (90u << 1) | 1u)

static ReplayStatus build_poster(const uint8_t *px, unsigned w, unsigned h,
                                 ReplayBuffer *out)
{
    unsigned row_bytes = w * 2U;
    unsigned words_per_row = (row_bytes + 3U) / 4U;
    unsigned padded = words_per_row * 4U;
    unsigned image_size = padded * h;
    unsigned sprite_payload = 44U + image_size;
    unsigned last_bytes = ((row_bytes + 3U) % 4U) + 1U; /* bytes used, last word */
    static const char name[12] = "poster";
    unsigned y;

    replay_buffer_clear(out);
    put_u32(out, 1U);                          /* number of sprites */
    put_u32(out, 16U);                         /* offset to first sprite */
    put_u32(out, 12U + sprite_payload + 4U);   /* offset to first free word */
    put_u32(out, sprite_payload);              /* offset to next sprite */
    if (replay_buffer_append(out, name, sizeof(name)) != REPLAY_OK) {
        return REPLAY_OUT_OF_MEMORY;
    }
    put_u32(out, words_per_row - 1U);          /* width in words - 1 */
    put_u32(out, h - 1U);                      /* height in scan lines - 1 */
    put_u32(out, 0U);                          /* first bit used */
    put_u32(out, last_bytes * 8U - 1U);        /* last bit used */
    put_u32(out, 44U);                         /* offset to image */
    put_u32(out, 44U);                         /* offset to mask (= image) */
    put_u32(out, POSTER_SPRITE_MODE);          /* 16bpp, square pixels */
    for (y = 0U; y < h; ++y) {
        static const uint8_t zero[4] = { 0, 0, 0, 0 };

        if (replay_buffer_append(out, px + (size_t)y * row_bytes, row_bytes) !=
                REPLAY_OK ||
            replay_buffer_append(out, zero, padded - row_bytes) != REPLAY_OK) {
            return REPLAY_OUT_OF_MEMORY;
        }
    }
    return REPLAY_OK;
}

int main(int argc, char **argv)
{
    const char *frames_prefix = NULL;
    const char *frame_suffix = ".mb19";
    const char *output_path = NULL;
    const char *pixel_label = NULL;
    const char *sound_path = NULL;
    const char *poster_path = NULL;
    unsigned poster_w = 0U;
    unsigned poster_h = 0U;
    int no_poster = 0;
    const char *sound_pcm_path = NULL;
    const char *sound_encode = "vidc-e8";
    const char *sound_format = "vidc-log";
    ReplayAe7WriteOptions options;
    ReplayAe7WriteTrack track;
    ReplayBuffer *frame_buffers = NULL;
    const uint8_t **frame_ptrs = NULL;
    size_t *frame_sizes = NULL;
    ReplayBuffer *key_buffers = NULL;
    const uint8_t **key_ptrs = NULL;
    const char *keys_prefix = NULL;
    ReplayBuffer sound_buffer;
    ReplayBuffer poster_buffer;
    ReplayBuffer sprite_buffer;
    ReplayBuffer movie_bytes;
    char error[256];
    char path[4096];
    unsigned width = 0U;
    unsigned height = 0U;
    unsigned pixel_depth = 16U;
    unsigned codec = 0U;
    unsigned frame_count = 0U;
    unsigned frames_per_chunk = 0U;
    unsigned align_mask = 0U;
    unsigned sound_rate = 11025U;
    unsigned sound_channels = 1U;
    unsigned sound_precision = 8U;
    double fps = 0.0;
    double chunk_seconds = 1.0;
    size_t i;
    int result = EXIT_FAILURE;
    int have_codec = 0;
    int arg;

    memset(&options, 0, sizeof(options));
    replay_buffer_init(&sound_buffer);
    replay_buffer_init(&poster_buffer);
    replay_buffer_init(&sprite_buffer);
    replay_buffer_init(&movie_bytes);

    for (arg = 1; arg < argc; ++arg) {
        const char *a = argv[arg];
        const char *value = arg + 1 < argc ? argv[arg + 1] : NULL;

#define NEED(name)                                                            \
    do {                                                                      \
        if (value == NULL) {                                                  \
            usage(stderr);                                                    \
            goto done;                                                        \
        }                                                                     \
        (void)(name);                                                         \
        ++arg;                                                                \
    } while (0)

        if (strcmp(a, "--codec") == 0) {
            NEED(value);
            codec = (unsigned)strtoul(value, NULL, 10);
            have_codec = 1;
        } else if (strcmp(a, "--size") == 0) {
            NEED(value);
            if (sscanf(value, "%ux%u", &width, &height) != 2) {
                usage(stderr);
                goto done;
            }
        } else if (strcmp(a, "--fps") == 0) {
            NEED(value);
            fps = strtod(value, NULL);
        } else if (strcmp(a, "--frames-prefix") == 0) {
            NEED(value);
            frames_prefix = value;
        } else if (strcmp(a, "--frame-suffix") == 0) {
            NEED(value);
            frame_suffix = value;
        } else if (strcmp(a, "--frames") == 0) {
            NEED(value);
            frame_count = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--frames-per-chunk") == 0) {
            NEED(value);
            frames_per_chunk = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--chunk-seconds") == 0) {
            NEED(value);
            chunk_seconds = strtod(value, NULL);
        } else if (strcmp(a, "--align") == 0) {
            NEED(value);
            align_mask = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--pixel-depth") == 0) {
            NEED(value);
            pixel_depth = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--pixel-label") == 0) {
            NEED(value);
            pixel_label = value;
        } else if (strcmp(a, "--output") == 0) {
            NEED(value);
            output_path = value;
        } else if (strcmp(a, "--title") == 0) {
            NEED(value);
            options.title = value;
        } else if (strcmp(a, "--copyright") == 0) {
            NEED(value);
            options.copyright = value;
        } else if (strcmp(a, "--author") == 0) {
            NEED(value);
            options.author = value;
        } else if (strcmp(a, "--sound") == 0) {
            NEED(value);
            sound_path = value;
        } else if (strcmp(a, "--keys-prefix") == 0) {
            NEED(value);
            keys_prefix = value;
        } else if (strcmp(a, "--poster") == 0) {
            NEED(value);
            poster_path = value;
        } else if (strcmp(a, "--poster-size") == 0) {
            NEED(value);
            if (sscanf(value, "%ux%u", &poster_w, &poster_h) != 2) {
                usage(stderr);
                goto done;
            }
        } else if (strcmp(a, "--no-poster") == 0) {
            no_poster = 1;
        } else if (strcmp(a, "--sound-pcm") == 0) {
            NEED(value);
            sound_pcm_path = value;
        } else if (strcmp(a, "--sound-encode") == 0) {
            NEED(value);
            sound_encode = value;
        } else if (strcmp(a, "--sound-rate") == 0) {
            NEED(value);
            sound_rate = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--sound-channels") == 0) {
            NEED(value);
            sound_channels = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--sound-precision") == 0) {
            NEED(value);
            sound_precision = (unsigned)strtoul(value, NULL, 10);
        } else if (strcmp(a, "--sound-format") == 0) {
            NEED(value);
            sound_format = value;
        } else {
            usage(stderr);
            goto done;
        }
#undef NEED
    }

    /* A sound-only movie has no video frames (codec 0) but needs a sound
     * source. A video movie needs frames and dimensions. */
    if (frame_count == 0U) {
        if (output_path == NULL || !(fps > 0.0) ||
            (sound_pcm_path == NULL && sound_path == NULL)) {
            usage(stderr);
            goto done;
        }
    } else if (!have_codec || frames_prefix == NULL || output_path == NULL ||
               width == 0U || height == 0U || !(fps > 0.0)) {
        usage(stderr);
        goto done;
    }

    if (frame_count != 0U) {
        frame_buffers = calloc(frame_count, sizeof(*frame_buffers));
        frame_ptrs = calloc(frame_count, sizeof(*frame_ptrs));
        frame_sizes = calloc(frame_count, sizeof(*frame_sizes));
        if (frame_buffers == NULL || frame_ptrs == NULL ||
            frame_sizes == NULL) {
            fprintf(stderr, "unable to allocate frame tables\n");
            goto done;
        }
    }
    for (i = 0U; i < frame_count; ++i) {
        int length = snprintf(path, sizeof(path), "%s%06zu%s", frames_prefix,
                              i, frame_suffix);

        if (length < 0 || (size_t)length >= sizeof(path)) {
            fprintf(stderr, "generated frame path is too long\n");
            goto done;
        }
        replay_buffer_init(&frame_buffers[i]);
        if (read_file(path, &frame_buffers[i]) != EXIT_SUCCESS) {
            goto done;
        }
        frame_ptrs[i] = frame_buffers[i].data;
        frame_sizes[i] = frame_buffers[i].size;
    }

    /* Optional per-frame key frames (from replay-encode --keys-prefix), one
     * native-format reconstruction per frame; the writer picks the chunk
     * boundaries. */
    if (keys_prefix != NULL && frame_count != 0U) {
        size_t key_size = (size_t)width * height * 2U;

        key_buffers = calloc(frame_count, sizeof(*key_buffers));
        key_ptrs = calloc(frame_count, sizeof(*key_ptrs));
        if (key_buffers == NULL || key_ptrs == NULL) {
            fprintf(stderr, "unable to allocate key tables\n");
            goto done;
        }
        for (i = 0U; i < frame_count; ++i) {
            int length = snprintf(path, sizeof(path), "%s%06zu.key",
                                  keys_prefix, i);

            if (length < 0 || (size_t)length >= sizeof(path)) {
                fprintf(stderr, "generated key path is too long\n");
                goto done;
            }
            replay_buffer_init(&key_buffers[i]);
            if (read_file(path, &key_buffers[i]) != EXIT_SUCCESS) {
                goto done;
            }
            if (key_buffers[i].size != key_size) {
                fprintf(stderr, "%s: key frame is %zu bytes, expected %zu\n",
                        path, key_buffers[i].size, key_size);
                goto done;
            }
            key_ptrs[i] = key_buffers[i].data;
        }
        options.write_keys = 1;
        options.key_data = key_ptrs;
        options.key_size = key_size;
    }

    if (sound_pcm_path != NULL) {
        /* Encode canonical signed-16 little-endian PCM (from ffmpeg) into a
         * Replay format-1 sub-format. The bits-per-sample label selects the
         * player's decoder. */
        ReplayBuffer pcm;
        ReplaySoundFormat fmt;
        size_t count;

        if (strcmp(sound_encode, "signed-8") == 0) {
            fmt = REPLAY_SOUND_SIGNED_8;
        } else if (strcmp(sound_encode, "signed-16") == 0) {
            fmt = REPLAY_SOUND_SIGNED_16;
        } else if (strcmp(sound_encode, "vidc-e8") == 0 ||
                   strcmp(sound_encode, "vidc-log") == 0) {
            fmt = REPLAY_SOUND_VIDC_E8;
        } else {
            fprintf(stderr, "unknown --sound-encode: %s\n", sound_encode);
            goto done;
        }
        replay_buffer_init(&pcm);
        if (read_file(sound_pcm_path, &pcm) != EXIT_SUCCESS) {
            replay_buffer_free(&pcm);
            goto done;
        }
        count = pcm.size / 2U; /* signed 16-bit little-endian samples */
        for (i = 0U; i < count; ++i) {
            int16_t sample = (int16_t)((uint16_t)pcm.data[i * 2U] |
                                       ((uint16_t)pcm.data[i * 2U + 1U] << 8));

            if (replay_sound_encode(fmt, &sample, 1U, &sound_buffer) !=
                REPLAY_OK) {
                fprintf(stderr, "audio encode failed\n");
                replay_buffer_free(&pcm);
                goto done;
            }
        }
        replay_buffer_free(&pcm);
        track.codec = REPLAY_AE7_SOUND_VIDC_LOG; /* format 1 */
        track.codec_name = NULL;
        track.rate_hz = sound_rate;
        track.channels = sound_channels;
        track.precision_bits = replay_sound_format_bits(fmt);
        track.label = replay_sound_format_label(fmt);
        track.data = sound_buffer.data;
        track.size = sound_buffer.size;
        options.tracks = &track;
        options.track_count = 1U;
    } else if (sound_path != NULL) {
        if (read_file(sound_path, &sound_buffer) != EXIT_SUCCESS) {
            goto done;
        }
        /* "vidc-log"/"vidc"/"vidc8" select the built-in 8-bit VIDC format 1;
         * any other name selects format 2 with that decompressor name (adpcm,
         * gsm, g721, g723-1, ...). */
        if (strcmp(sound_format, "vidc-log") == 0 ||
            strcmp(sound_format, "vidc") == 0 ||
            strcmp(sound_format, "vidc8") == 0) {
            track.codec = REPLAY_AE7_SOUND_VIDC_LOG;
            track.codec_name = NULL;
        } else {
            track.codec = REPLAY_AE7_SOUND_NAMED;
            track.codec_name = sound_format;
        }
        track.rate_hz = sound_rate;
        track.channels = sound_channels;
        track.precision_bits = sound_precision;
        track.label = NULL;
        track.data = sound_buffer.data;
        track.size = sound_buffer.size;
        options.tracks = &track;
        options.track_count = 1U;
    }

    options.video_codec = codec;
    options.width = width;
    options.height = height;
    options.pixel_depth = pixel_depth;
    options.pixel_label = pixel_label;
    options.frames_per_second = fps;
    options.frame_data = frame_ptrs;
    options.frame_size = frame_sizes;
    options.frame_count = frame_count;
    options.frames_per_chunk = frames_per_chunk;
    options.chunk_seconds = chunk_seconds;
    options.align_mask = align_mask;

    if (poster_path != NULL) {
        size_t want;

        if (poster_w == 0U || poster_h == 0U) {
            poster_w = width;
            poster_h = height;
        }
        want = (size_t)poster_w * poster_h * 2U;
        if (read_file(poster_path, &poster_buffer) != EXIT_SUCCESS) {
            goto done;
        }
        if (poster_buffer.size != want) {
            fprintf(stderr,
                    "%s: poster must be %ux%u bgr555 pixels (%zu bytes), got %zu\n",
                    poster_path, poster_w, poster_h, want, poster_buffer.size);
            goto done;
        }
        if (build_poster(poster_buffer.data, poster_w, poster_h,
                         &sprite_buffer) != REPLAY_OK) {
            fprintf(stderr, "unable to build poster sprite\n");
            goto done;
        }
        options.sprite_data = sprite_buffer.data;
        options.sprite_size = sprite_buffer.size;
    } else if (!no_poster) {
        /* No custom poster: embed the built-in Replay-logo default so the movie
         * still opens in !ARPlayer (which crashes on a missing sprite). */
        options.sprite_data = replay_default_poster;
        options.sprite_size = replay_default_poster_size;
    }

    if (replay_ae7_write(&options, &movie_bytes, error, sizeof(error)) !=
        REPLAY_OK) {
        fprintf(stderr, "%s: %s\n", output_path, error);
        goto done;
    }
    {
        FILE *out = fopen(output_path, "wb");

        if (out == NULL) {
            perror(output_path);
            goto done;
        }
        if (fwrite(movie_bytes.data, 1U, movie_bytes.size, out) !=
                movie_bytes.size ||
            fclose(out) != 0) {
            perror(output_path);
            goto done;
        }
    }
    printf("codec=%u size=%ux%u frames=%u fps=%g bytes=%zu output=\"%s\"\n",
           codec, width, height, frame_count, fps, movie_bytes.size,
           output_path);
    result = EXIT_SUCCESS;

done:
    if (frame_buffers != NULL) {
        for (i = 0U; i < frame_count; ++i) {
            replay_buffer_free(&frame_buffers[i]);
        }
        free(frame_buffers);
    }
    if (key_buffers != NULL) {
        for (i = 0U; i < frame_count; ++i) {
            replay_buffer_free(&key_buffers[i]);
        }
        free(key_buffers);
    }
    free(key_ptrs);
    free(frame_ptrs);
    free(frame_sizes);
    replay_buffer_free(&sound_buffer);
    replay_buffer_free(&poster_buffer);
    replay_buffer_free(&sprite_buffer);
    replay_buffer_free(&movie_bytes);
    return result;
}
