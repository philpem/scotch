#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_movingblocks.h"
#include "replay/codec_movingblocksbeta.h"
#include "replay/codec_movingblockshq.h"
#include "replay/codec_movinglines.h"
#include "replay/codec_supermovingblocks.h"
#include "replay/mb_color.h"
#include "replay/mb_metrics.h"
#include "replay/mb_quality.h"
#include "replay/mb_rate_control.h"
#include "replay/mb_repeat.h"
#include "replay/replay_ae7_write.h"
#include "replay/replay_movie.h"

#include "default_poster.h"

/*
 * Re-decode every encoded frame and compare it to the encoder's own
 * reconstruction. This was a development gate to prove the encode and decode
 * paths agree; the codecs are now cross-checked byte-exact against the real
 * Acorn Decomp modules in the test suite, so it is redundant and off by
 * default. --verify re-enables it when developing or debugging the encoder.
 * (It is a single decode pass, ~2% of encode time -- the cost is the per-block
 * motion/spatial search in the encoder, not this.)
 */
static int self_check_frames = 0;

/*
 * This program is intentionally a thin development harness around the codec:
 *
 *   RGB24 frame -> CompLib-style 6Y5UV --\
 *   packed 6Y5UV -------------------------> encode -> independent decode
 *                                        -> compare -> write raw payload
 *
 * Raw payloads are written separately because concatenating variable-length
 * frames would invent a container unrelated to Replay. The later Replay
 * writer will provide timing, frame boundaries, and key-frame metadata.
 */
typedef enum {
    FRAME_READ_OK,
    FRAME_READ_EOF,
    FRAME_READ_ERROR
} FrameReadResult;

typedef enum {
    INPUT_RGB24,
    INPUT_6Y5UV,
    INPUT_YUV555
} InputFormat;

typedef enum {
    RATE_SEARCH_LINEAR,
    RATE_SEARCH_BRACKETED
} RateSearch;

typedef struct {
    int direction;
    unsigned low;
    unsigned high;
    unsigned candidate;
    unsigned step;
    int have_candidate;
    int final_probe;
} BracketedRateSearch;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-encode --codec 1|7|17|19|20 --input FILE|- --size WxH "
            "(--payload FILE | --payload-prefix PREFIX | --output MOVIE,ae7) "
            "[--input-format rgb24|6y5uv|yuv555] [--dither 4x4|8x8|--no-dither] "
            "[--frames N] [--data-only] [--verify] [--loss-level 0..28] "
            "[--policy ordered|lowest-error] [--variant old|new] "
            "[--rate-search linear|bracketed] "
            "[--target-bytes N] [--pad-to-multiple N] [--trace FILE] "
            "[--recon-ppm FILE | --recon-prefix PREFIX] "
            "[--keys-prefix PREFIX]\n"
            "  codec 1 (Moving Lines) takes --input (RGB24), --size, --frames, "
            "--colour rgb|yuv, --payload[-prefix], --recon[-ppm|-prefix] and "
            "--output.\n"
            "  direct-to-container (--output) also takes:\n"
            "    --fps F --frames-per-chunk N [--container-type N] "
            "[--pixel-label L] [--pixel-depth N] [--align MASK] [--keys]\n"
            "    [--audio-input FILE|- --sound-encode vidc-e8|signed-8|signed-16|"
            "adpcm|adpcm2 --sound-rate HZ --sound-channels N]\n"
            "    [--poster FILE.bgr555 [--poster-size WxH] | --no-poster] "
            "[--title T] [--copyright C] [--author A]\n"
            "    (--payload-prefix may be added to also dump per-frame payloads "
            "for debugging)\n");
}

static int bracketed_rate_next(BracketedRateSearch *search,
                               const MbRateControl *control,
                               size_t encoded_bytes, unsigned current,
                               unsigned *next)
{
    /*
     * Higher loss levels are treated as producing no more bytes than lower
     * levels. Start with the adjacent row so a carried level stays cheap,
     * expand exponentially until the target boundary is crossed, then use a
     * binary search for the first row on that boundary. The final payload is
     * still produced by the ordinary encoder and independently decoded.
     */
    if (search->final_probe) {
        return 0;
    }
    if (search->direction == 0) {
        if (encoded_bytes >= control->target_min_bytes &&
            encoded_bytes <= control->target_max_bytes) {
            return 0;
        }
        if (encoded_bytes > control->target_max_bytes) {
            if (current + 1U >= MB_QUALITY_LEVEL_COUNT) {
                return 0;
            }
            search->direction = 1;
            search->low = current + 1U;
            search->high = MB_QUALITY_LEVEL_COUNT - 1U;
            search->step = 2U;
            *next = search->low;
        } else {
            if (current == 0U) {
                return 0;
            }
            search->direction = -1;
            search->low = 0U;
            search->high = current - 1U;
            search->step = 2U;
            *next = search->high;
        }
        return 1;
    }

    if (search->direction > 0) {
        if (encoded_bytes <= control->target_max_bytes) {
            search->candidate = current;
            search->have_candidate = 1;
            if (current == 0U) {
                search->high = 0U;
            } else {
                search->high = current - 1U;
            }
        } else {
            search->low = current + 1U;
        }
    } else {
        if (encoded_bytes >= control->target_min_bytes) {
            search->candidate = current;
            search->have_candidate = 1;
            search->low = current + 1U;
        } else if (current == 0U) {
            return 0;
        } else {
            search->high = current - 1U;
        }
    }

    if (search->have_candidate) {
        if (search->low <= search->high) {
            *next = search->direction > 0
                        ? search->low + (search->high - search->low) / 2U
                        : search->low +
                              (search->high - search->low + 1U) / 2U;
            return 1;
        }
        *next = search->candidate;
        if (*next == current) {
            return 0;
        }
        search->final_probe = 1;
        return 1;
    }

    if (search->direction > 0) {
        unsigned remaining = MB_QUALITY_LEVEL_COUNT - 1U - current;

        if (remaining == 0U) {
            return 0;
        }
        *next = current + (search->step < remaining
                               ? search->step : remaining);
    } else {
        if (current == 0U) {
            return 0;
        }
        *next = current > search->step ? current - search->step : 0U;
    }
    if (search->step <= (MB_QUALITY_LEVEL_COUNT - 1U) / 2U) {
        search->step *= 2U;
    }
    if (*next == current) {
        return 0;
    }
    return 1;
}

static FrameReadResult read_frame(FILE *file, const char *name, uint8_t *data,
                                  size_t size)
{
    size_t offset = 0U;

    while (offset < size) {
        size_t count = fread(data + offset, 1U, size - offset, file);
        if (count == 0U) {
            if (ferror(file)) {
                perror(name);
                return FRAME_READ_ERROR;
            }
            /* EOF between frames is normal; EOF inside a frame is corruption. */
            if (offset == 0U) {
                return FRAME_READ_EOF;
            }
            fprintf(stderr, "%s: truncated input frame (%zu of %zu bytes)\n",
                    name, offset, size);
            return FRAME_READ_ERROR;
        }
        offset += count;
    }
    return FRAME_READ_OK;
}

static ReplayStatus unpack_6y5uv(const uint8_t *packed, MbFrame *frame)
{
    unsigned y;

    for (y = 0U; y < frame->height; ++y) {
        unsigned x;

        for (x = 0U; x < frame->width; ++x) {
            size_t input_offset =
                ((size_t)y * (size_t)frame->width + (size_t)x) * 3U;
            MbPixel *pixel = &frame->pixels[(size_t)y * frame->stride + x];

            pixel->y = packed[input_offset];
            pixel->u = packed[input_offset + 1U];
            pixel->v = packed[input_offset + 2U];
            if (pixel->y > 63U || pixel->u > 31U || pixel->v > 31U) {
                return REPLAY_MALFORMED_STREAM;
            }
        }
    }
    return REPLAY_OK;
}

static int require_eof(FILE *file, const char *name)
{
    uint8_t extra;

    if (fread(&extra, 1U, 1U, file) != 0U) {
        fprintf(stderr, "%s: input contains more frames than requested\n",
                name);
        return EXIT_FAILURE;
    }
    if (ferror(file)) {
        perror(name);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int make_frame_path(char *path, size_t path_size, const char *prefix,
                           size_t frame_number, const char *suffix)
{
    int length = snprintf(path, path_size, "%s%06zu%s", prefix,
                          frame_number, suffix);

    if (length < 0 || (size_t)length >= path_size) {
        fprintf(stderr, "generated frame path is too long\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int write_payload(const char *path, const ReplayBuffer *payload)
{
    FILE *file = fopen(path, "wb");
    int result = EXIT_SUCCESS;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fwrite(payload->data, 1U, payload->size, file) != payload->size) {
        perror(path);
        result = EXIT_FAILURE;
    }
    if (fclose(file) != 0 && result == EXIT_SUCCESS) {
        perror(path);
        result = EXIT_FAILURE;
    }
    return result;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    int result = EXIT_SUCCESS;
    uint8_t block[65536];

    if (in == NULL) {
        perror(src);
        return EXIT_FAILURE;
    }
    out = fopen(dst, "wb");
    if (out == NULL) {
        perror(dst);
        fclose(in);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(block, 1U, sizeof(block), in);

        if (count != 0U && fwrite(block, 1U, count, out) != count) {
            perror(dst);
            result = EXIT_FAILURE;
            break;
        }
        if (count != sizeof(block)) {
            if (ferror(in)) {
                perror(src);
                result = EXIT_FAILURE;
            }
            break;
        }
    }
    if (fclose(in) != 0 && result == EXIT_SUCCESS) {
        perror(src);
        result = EXIT_FAILURE;
    }
    if (fclose(out) != 0 && result == EXIT_SUCCESS) {
        perror(dst);
        result = EXIT_FAILURE;
    }
    return result;
}

/*
 * Pad the encoded sequence up to a multiple of `multiple` frames by appending
 * "repeat last frame" payloads -- a frame in which every block is stationary, so
 * the decoder reproduces the previous reconstruction unchanged. The player
 * decodes a fixed number of frames per chunk, so a partial final chunk would
 * otherwise be filled with decode garbage; Acorn's own compressor pads the same
 * way ("repeating last frame"). Only meaningful when writing a movie's per-frame
 * payloads (payload_prefix set) and after at least one real frame. When key
 * frames are emitted, each pad frame reuses the previous frame's key (which the
 * repeat frame reproduces). On return *frame_number counts the pad frames too.
 */
/* Forward declarations: the padding writer feeds the in-memory frame sink used
 * by the direct-to-container path; both are fully defined below. */
typedef struct FrameSink FrameSink;
static int frame_sink_add(FrameSink *sink, const ReplayBuffer *payload,
                          const MbFrame *key_recon);

static int write_pad_frames(unsigned codec, unsigned width, unsigned height,
                            const char *payload_prefix,
                            const char *payload_suffix, const char *keys_prefix,
                            unsigned multiple, size_t *frame_number,
                            FrameSink *sink)
{
    ReplayBuffer payload;
    size_t pad_count;
    size_t i;
    int result = EXIT_FAILURE;

    if ((payload_prefix == NULL && sink == NULL) || multiple <= 1U ||
        *frame_number == 0U || *frame_number % multiple == 0U) {
        return EXIT_SUCCESS;
    }
    pad_count = multiple - (*frame_number % multiple);

    replay_buffer_init(&payload);
    if (mb_repeat_payload(codec, width, height, &payload) != REPLAY_OK) {
        fprintf(stderr, "cannot build repeat frame for codec %u\n", codec);
        goto done;
    }
    for (i = 0U; i < pad_count; ++i) {
        if (payload_prefix != NULL) {
            char path[1024];

            if (make_frame_path(path, sizeof(path), payload_prefix,
                                *frame_number, payload_suffix) != EXIT_SUCCESS ||
                write_payload(path, &payload) != EXIT_SUCCESS) {
                goto done;
            }
            if (keys_prefix != NULL) {
                char prev_key[1024];
                char pad_key[1024];

                if (make_frame_path(prev_key, sizeof(prev_key), keys_prefix,
                                    *frame_number - 1U, ".key") != EXIT_SUCCESS ||
                    make_frame_path(pad_key, sizeof(pad_key), keys_prefix,
                                    *frame_number, ".key") != EXIT_SUCCESS ||
                    copy_file(prev_key, pad_key) != EXIT_SUCCESS) {
                    goto done;
                }
            }
        }
        /* A pad frame repeats the previous frame, so its key is the previous
         * key (key_recon == NULL tells the sink to reuse it). */
        if (sink != NULL && frame_sink_add(sink, &payload, NULL) != EXIT_SUCCESS) {
            goto done;
        }
        ++*frame_number;
    }
    result = EXIT_SUCCESS;
done:
    replay_buffer_free(&payload);
    return result;
}

static int write_reconstructed_ppm(const char *path, const MbFrame *frame,
                                   uint8_t *rgb, size_t rgb_stride)
{
    FILE *file;
    ReplayStatus status;

    if (path == NULL) {
        return EXIT_SUCCESS;
    }
    status = mb_color_6y5uv_to_rgb24(frame, rgb, rgb_stride);
    if (status != REPLAY_OK) {
        fprintf(stderr, "reconstructed RGB conversion failed: %s\n",
                replay_status_string(status));
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", frame->width, frame->height) < 0 ||
        fwrite(rgb, rgb_stride, frame->height, file) != frame->height) {
        perror(path);
        fclose(file);
        return EXIT_FAILURE;
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*
 * Pack a reconstructed frame into the player's native key-frame halfwords --
 * 6Y5UV (Y[0:5], U[6:10], V[11:15]) for type 19, YUV555 (Y[0:4], U[5:9],
 * V[10:14]) for types 7/17, little-endian -- the format the player expands when
 * starting decompression at a chunk boundary. (Type 20 does not support keys.)
 */
static ReplayStatus pack_key_frame(unsigned codec, const MbFrame *frame,
                                   ReplayBuffer *out)
{
    unsigned y_mask = codec == 19U ? 0x3FU : 0x1FU;
    unsigned u_shift = codec == 19U ? 6U : 5U;
    unsigned v_shift = codec == 19U ? 11U : 10U;
    unsigned y;

    replay_buffer_clear(out);
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;

        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *p = &frame->pixels[(size_t)y * frame->stride + x];
            unsigned packed = ((unsigned)p->y & y_mask) |
                              (((unsigned)p->u & 0x1FU) << u_shift) |
                              (((unsigned)p->v & 0x1FU) << v_shift);
            uint8_t bytes[2] = {
                (uint8_t)(packed & 0xFFU), (uint8_t)((packed >> 8U) & 0xFFU)
            };

            if (replay_buffer_append(out, bytes, sizeof(bytes)) != REPLAY_OK) {
                return REPLAY_OUT_OF_MEMORY;
            }
        }
    }
    return REPLAY_OK;
}

static int write_key_file(const char *path, unsigned codec,
                          const MbFrame *frame)
{
    ReplayBuffer key;
    int result = EXIT_SUCCESS;

    replay_buffer_init(&key);
    if (pack_key_frame(codec, frame, &key) != REPLAY_OK) {
        fprintf(stderr, "unable to pack key frame\n");
        replay_buffer_free(&key);
        return EXIT_FAILURE;
    }
    result = write_payload(path, &key);
    replay_buffer_free(&key);
    return result;
}

/* Type 19 keys are 6Y5UV; types 7/17 keys are YUV555. */
static int write_key_frame(const char *path, const MbFrame *frame)
{
    return write_key_file(path, 19U, frame);
}

static int write_key_frame_yuv555(const char *path, const MbFrame *frame)
{
    return write_key_file(path, 17U, frame);
}

/*
 * Collects encoded frame payloads (and, when keys are wanted, packed key frames)
 * in memory so the direct-to-container path (--output) can hand them to
 * replay_ae7_write without writing per-frame temp files. Same memory profile as
 * replay-join, which already buffers every frame.
 */
struct FrameSink {
    ReplayBuffer *payloads;
    ReplayBuffer *keys; /* parallel to payloads; used only when want_keys */
    size_t count;
    size_t capacity;
    unsigned codec;
    int want_keys;
};

static void frame_sink_init(FrameSink *sink, unsigned codec, int want_keys)
{
    sink->payloads = NULL;
    sink->keys = NULL;
    sink->count = 0U;
    sink->capacity = 0U;
    sink->codec = codec;
    sink->want_keys = want_keys;
}

static int frame_sink_reserve(FrameSink *sink)
{
    size_t newcap;
    ReplayBuffer *grown;

    if (sink->count < sink->capacity) {
        return EXIT_SUCCESS;
    }
    newcap = sink->capacity != 0U ? sink->capacity * 2U : 64U;
    grown = realloc(sink->payloads, newcap * sizeof(*grown));
    if (grown == NULL) {
        return EXIT_FAILURE;
    }
    sink->payloads = grown;
    if (sink->want_keys) {
        grown = realloc(sink->keys, newcap * sizeof(*grown));
        if (grown == NULL) {
            return EXIT_FAILURE;
        }
        sink->keys = grown;
    }
    sink->capacity = newcap;
    return EXIT_SUCCESS;
}

/*
 * Append one frame. `key_recon` is the reconstruction to pack as this frame's
 * key, or NULL for a padding frame (which repeats the previous frame's key).
 */
static int frame_sink_add(FrameSink *sink, const ReplayBuffer *payload,
                          const MbFrame *key_recon)
{
    ReplayBuffer *slot;

    if (frame_sink_reserve(sink) != EXIT_SUCCESS) {
        fprintf(stderr, "out of memory collecting frames\n");
        return EXIT_FAILURE;
    }
    slot = &sink->payloads[sink->count];
    replay_buffer_init(slot);
    if (replay_buffer_append(slot, payload->data, payload->size) != REPLAY_OK) {
        fprintf(stderr, "out of memory collecting frames\n");
        return EXIT_FAILURE;
    }
    if (sink->want_keys) {
        ReplayBuffer *key = &sink->keys[sink->count];

        replay_buffer_init(key);
        if (key_recon != NULL) {
            if (pack_key_frame(sink->codec, key_recon, key) != REPLAY_OK) {
                fprintf(stderr, "out of memory collecting frames\n");
                return EXIT_FAILURE;
            }
        } else if (sink->count > 0U) {
            const ReplayBuffer *prev = &sink->keys[sink->count - 1U];

            if (replay_buffer_append(key, prev->data, prev->size) != REPLAY_OK) {
                fprintf(stderr, "out of memory collecting frames\n");
                return EXIT_FAILURE;
            }
        }
    }
    ++sink->count;
    return EXIT_SUCCESS;
}

static void frame_sink_free(FrameSink *sink)
{
    size_t i;

    for (i = 0U; i < sink->count; ++i) {
        replay_buffer_free(&sink->payloads[i]);
        if (sink->want_keys) {
            replay_buffer_free(&sink->keys[i]);
        }
    }
    free(sink->payloads);
    free(sink->keys);
    sink->payloads = NULL;
    sink->keys = NULL;
    sink->count = 0U;
    sink->capacity = 0U;
}

static int trace_frame(FILE *trace, size_t frame_number, unsigned width,
                       unsigned height,
                       const CodecSuperMovingBlocksEncodeStats *stats,
                       const MbFrame *source, const MbFrame *reconstructed,
                       size_t bytes, unsigned loss_level, unsigned retry,
                       size_t target_min, size_t target_max,
                       CodecSuperMovingBlocksPolicy policy,
                       RateSearch rate_search)
{
    MbFrameMetrics metrics;

    if (trace == NULL) {
        return EXIT_SUCCESS;
    }
    if (mb_metrics_compare_6y5uv(source, reconstructed, &metrics) !=
        REPLAY_OK) {
        fprintf(stderr, "unable to calculate 6Y5UV quality metrics\n");
        return EXIT_FAILURE;
    }
    if (fprintf(trace,
                "frame=%zu codec=19 size=%ux%u retry=%u loss_level=%u "
                "name=\"Super Moving Blocks\" "
                "policy=%s rate_search=%s "
                "target_min=%zu target_max=%zu data4x4=%zu "
                "stationary4x4=%zu temporal4x4=%zu spatial4x4=%zu "
                "split4x4=%zu data2x2=%zu stationary2x2=%zu "
                "temporal2x2=%zu spatial2x2=%zu "
                "eval_stationary4x4=%zu eval_temporal4x4=%zu "
                "eval_spatial4x4=%zu eval_stationary2x2=%zu "
                "eval_temporal2x2=%zu eval_spatial2x2=%zu "
                "bits=%zu bytes=%zu "
                "sse_y=%llu sse_u=%llu sse_v=%llu "
                "mse_y=%.6f mse_u=%.6f mse_v=%.6f "
                "psnr_y=%.6f psnr_u=%.6f psnr_v=%.6f "
                "max_error_y=%u max_error_u=%u max_error_v=%u "
                "verify=ok\n",
                frame_number, width, height, retry, loss_level,
                policy == CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR
                    ? "lowest-error" : "ordered",
                rate_search == RATE_SEARCH_BRACKETED ? "bracketed" : "linear",
                target_min, target_max, stats->data4x4_blocks,
                stats->stationary4x4_blocks, stats->temporal4x4_blocks,
                stats->spatial4x4_blocks, stats->split4x4_blocks,
                stats->data2x2_blocks, stats->stationary2x2_blocks,
                stats->temporal2x2_blocks, stats->spatial2x2_blocks,
                stats->stationary4x4_evaluations,
                stats->temporal4x4_evaluations,
                stats->spatial4x4_evaluations,
                stats->stationary2x2_evaluations,
                stats->temporal2x2_evaluations,
                stats->spatial2x2_evaluations,
                stats->bits_written, bytes,
                (unsigned long long)metrics.squared_error_y,
                (unsigned long long)metrics.squared_error_u,
                (unsigned long long)metrics.squared_error_v,
                mb_metrics_mse(metrics.squared_error_y, metrics.pixel_count),
                mb_metrics_mse(metrics.squared_error_u, metrics.pixel_count),
                mb_metrics_mse(metrics.squared_error_v, metrics.pixel_count),
                mb_metrics_psnr(metrics.squared_error_y,
                                metrics.pixel_count, 63U),
                mb_metrics_psnr(metrics.squared_error_u,
                                metrics.pixel_count, 31U),
                mb_metrics_psnr(metrics.squared_error_v,
                                metrics.pixel_count, 31U),
                metrics.max_error_y, metrics.max_error_u,
                metrics.max_error_v) < 0) {
        perror("trace output");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static ReplayStatus unpack_yuv555(const uint8_t *packed, MbFrame *frame)
{
    unsigned y;

    for (y = 0U; y < frame->height; ++y) {
        unsigned x;

        for (x = 0U; x < frame->width; ++x) {
            size_t input_offset =
                ((size_t)y * (size_t)frame->width + (size_t)x) * 3U;
            MbPixel *pixel = &frame->pixels[(size_t)y * frame->stride + x];

            pixel->y = packed[input_offset];
            pixel->u = packed[input_offset + 1U];
            pixel->v = packed[input_offset + 2U];
            if (pixel->y > 31U || pixel->u > 31U || pixel->v > 31U) {
                return REPLAY_MALFORMED_STREAM;
            }
        }
    }
    return REPLAY_OK;
}

static int write_yuv555_ppm(const char *path, const MbFrame *frame,
                            uint8_t *rgb, size_t rgb_stride)
{
    FILE *file;

    if (path == NULL) {
        return EXIT_SUCCESS;
    }
    if (mb_color_yuv555_to_rgb24(frame, rgb, rgb_stride) != REPLAY_OK) {
        fprintf(stderr, "reconstructed RGB conversion failed\n");
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", frame->width, frame->height) < 0 ||
        fwrite(rgb, rgb_stride, frame->height, file) != frame->height) {
        perror(path);
        fclose(file);
        return EXIT_FAILURE;
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* As write_yuv555_ppm, but for type 20's 6Y6UV reconstruction. */
static int write_6y6uv_ppm(const char *path, const MbFrame *frame,
                           uint8_t *rgb, size_t rgb_stride)
{
    FILE *file;

    if (path == NULL) {
        return EXIT_SUCCESS;
    }
    if (mb_color_6y6uv_to_rgb24(frame, rgb, rgb_stride) != REPLAY_OK) {
        fprintf(stderr, "reconstructed RGB conversion failed\n");
        return EXIT_FAILURE;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", frame->width, frame->height) < 0 ||
        fwrite(rgb, rgb_stride, frame->height, file) != frame->height) {
        perror(path);
        fclose(file);
        return EXIT_FAILURE;
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------------- *
 * Unified Moving Blocks encode driver (types 7, 17, 20).
 *
 * These three codecs share one encode loop: read a frame, convert it to the
 * codec's working colour space, run the device-bandwidth rate-control retry
 * loop, optionally self-check by independent decode, then emit the payload and
 * any requested reconstruction/key side outputs. Only a handful of per-codec
 * details vary, captured in MbToolCodec below. (Type 19 keeps its own loop in
 * main(): it additionally supports --trace and the bracketed rate search.)
 * ------------------------------------------------------------------------- */

/* The per-frame block tally the stats line prints, shared by all three codecs. */
typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
    size_t spatial4x4_blocks;
    size_t split4x4_blocks;
    size_t data2x2_blocks;
    size_t stationary2x2_blocks;
    size_t temporal2x2_blocks;
    size_t spatial2x2_blocks;
    size_t bits_written;
} MbToolStats;

#define MB_TOOL_COPY_STATS(dst, src)                                      \
    do {                                                                  \
        (dst).data4x4_blocks = (src).data4x4_blocks;                      \
        (dst).stationary4x4_blocks = (src).stationary4x4_blocks;          \
        (dst).temporal4x4_blocks = (src).temporal4x4_blocks;              \
        (dst).spatial4x4_blocks = (src).spatial4x4_blocks;                \
        (dst).split4x4_blocks = (src).split4x4_blocks;                    \
        (dst).data2x2_blocks = (src).data2x2_blocks;                      \
        (dst).stationary2x2_blocks = (src).stationary2x2_blocks;          \
        (dst).temporal2x2_blocks = (src).temporal2x2_blocks;              \
        (dst).spatial2x2_blocks = (src).spatial2x2_blocks;                \
        (dst).bits_written = (src).bits_written;                          \
    } while (0)

/*
 * Per-codec hooks for the shared driver. The encode/verify wrappers translate
 * the codec-neutral MbEncodeOptions and block tally to and from each codec's
 * own option/stats structs; `variant` is used only by type 20.
 */
typedef ReplayStatus (*MbToolEncodeFn)(
    const MbFrame *source, const MbFrame *previous,
    const MbEncodeOptions *options, int variant, ReplayBuffer *output,
    MbFrame *reconstructed, MbToolStats *stats);

typedef ReplayStatus (*MbToolVerifyFn)(const uint8_t *payload,
                                       size_t payload_size,
                                       const MbFrame *previous,
                                       MbFrame *decoded, int variant);

typedef struct {
    unsigned number;         /* compression type number, for messages/stats */
    const char *name;        /* display name in the stats line */
    const char *payload_ext; /* per-frame payload file extension */
    int rgb_only;            /* reject non-RGB24 input (type 20) */
    int supports_keys;       /* allow --keys-prefix (not type 20) */
    ReplayStatus (*from_rgb24)(const uint8_t *rgb, size_t rgb_stride,
                               MbFrame *output, MbColorDither dither);
    ReplayStatus (*unpack)(const uint8_t *packed, MbFrame *frame);
    int (*write_ppm)(const char *path, const MbFrame *frame, uint8_t *scratch,
                     size_t scratch_stride);
    int (*write_key)(const char *path, const MbFrame *frame);
    MbToolEncodeFn encode;
    MbToolVerifyFn verify;
} MbToolCodec;

static ReplayStatus mb_tool_encode7(const MbFrame *source,
                                    const MbFrame *previous,
                                    const MbEncodeOptions *options, int variant,
                                    ReplayBuffer *output, MbFrame *reconstructed,
                                    MbToolStats *stats)
{
    CodecMovingBlocksEncodeOptions o;
    CodecMovingBlocksEncodeStats s;
    ReplayStatus status;

    (void)variant;
    o.allow_stationary = options->allow_stationary;
    o.allow_temporal = options->allow_temporal;
    o.allow_spatial = options->allow_spatial;
    o.allow_split = options->allow_split;
    o.loss_level = options->loss_level;
    o.policy = options->policy;
    o.workspace = options->workspace;
    status = codec_movingblocks_encode_frame(source, previous, &o, output,
                                             reconstructed, &s);
    if (status == REPLAY_OK) {
        MB_TOOL_COPY_STATS(*stats, s);
    }
    return status;
}

static ReplayStatus mb_tool_encode17(const MbFrame *source,
                                     const MbFrame *previous,
                                     const MbEncodeOptions *options, int variant,
                                     ReplayBuffer *output,
                                     MbFrame *reconstructed, MbToolStats *stats)
{
    CodecMovingBlocksHqEncodeOptions o;
    CodecMovingBlocksHqEncodeStats s;
    ReplayStatus status;

    (void)variant;
    o.allow_stationary = options->allow_stationary;
    o.allow_temporal = options->allow_temporal;
    o.allow_spatial = options->allow_spatial;
    o.allow_split = options->allow_split;
    o.loss_level = options->loss_level;
    o.policy = options->policy;
    o.workspace = options->workspace;
    status = codec_movingblockshq_encode_frame(source, previous, &o, output,
                                               reconstructed, &s);
    if (status == REPLAY_OK) {
        MB_TOOL_COPY_STATS(*stats, s);
    }
    return status;
}

static ReplayStatus mb_tool_encode20(const MbFrame *source,
                                     const MbFrame *previous,
                                     const MbEncodeOptions *options, int variant,
                                     ReplayBuffer *output,
                                     MbFrame *reconstructed, MbToolStats *stats)
{
    CodecMovingBlocksBetaEncodeOptions o;
    CodecMovingBlocksBetaEncodeStats s;
    ReplayStatus status;

    o.allow_stationary = options->allow_stationary;
    o.allow_temporal = options->allow_temporal;
    o.allow_spatial = options->allow_spatial;
    o.allow_split = options->allow_split;
    o.loss_level = options->loss_level;
    o.policy = options->policy;
    o.workspace = options->workspace;
    o.variant = (CodecMovingBlocksBetaVariant)variant;
    status = codec_movingblocksbeta_encode_frame(source, previous, &o, output,
                                                 reconstructed, &s);
    if (status == REPLAY_OK) {
        MB_TOOL_COPY_STATS(*stats, s);
    }
    return status;
}

static ReplayStatus mb_tool_verify7(const uint8_t *payload, size_t payload_size,
                                    const MbFrame *previous, MbFrame *decoded,
                                    int variant)
{
    (void)variant;
    return codec_movingblocks_verify_frame(payload, payload_size, previous,
                                           decoded, NULL, NULL);
}

static ReplayStatus mb_tool_verify17(const uint8_t *payload,
                                     size_t payload_size,
                                     const MbFrame *previous, MbFrame *decoded,
                                     int variant)
{
    (void)variant;
    return codec_movingblockshq_verify_frame(payload, payload_size, previous,
                                             decoded, NULL, NULL);
}

static ReplayStatus mb_tool_verify20(const uint8_t *payload,
                                     size_t payload_size,
                                     const MbFrame *previous, MbFrame *decoded,
                                     int variant)
{
    return codec_movingblocksbeta_verify_frame_variant(
        payload, payload_size, previous, decoded, NULL, NULL,
        (CodecMovingBlocksBetaVariant)variant);
}

static const MbToolCodec mb_tool_codec7 = {
    7U, "Moving Blocks", ".mb7", 0, 1,
    mb_color_rgb24_to_yuv555, unpack_yuv555,
    write_yuv555_ppm, write_key_frame_yuv555,
    mb_tool_encode7, mb_tool_verify7
};

static const MbToolCodec mb_tool_codec17 = {
    17U, "Moving Blocks HQ", ".mb17", 0, 1,
    mb_color_rgb24_to_yuv555, unpack_yuv555,
    write_yuv555_ppm, write_key_frame_yuv555,
    mb_tool_encode17, mb_tool_verify17
};

static const MbToolCodec mb_tool_codec20 = {
    20U, "Moving Blocks Beta", ".mb20", 1, 0,
    mb_color_rgb24_to_6y6uv, NULL,
    write_6y6uv_ppm, NULL,
    mb_tool_encode20, mb_tool_verify20
};

/*
 * The shared encode loop for types 7, 17 and 20. Behaviour matches the former
 * per-codec run_encodeN functions exactly; only the per-codec specifics come
 * from `codec`. `variant` is forwarded to the type-20 hooks and ignored by the
 * others.
 */
static int run_mb_encode(const MbToolCodec *codec, int variant,
                         const char *input_path, InputFormat input_format,
                         unsigned width, unsigned height,
                         const char *payload_path, const char *payload_prefix,
                         size_t frame_limit, unsigned loss_level, int data_only,
                         const char *recon_prefix, const char *recon_ppm_path,
                         const char *keys_prefix, size_t target_bytes,
                         MbColorDither dither, MbEncodePolicy policy,
                         unsigned pad_to_multiple, FrameSink *sink)
{
    size_t pixel_count = (size_t)width * (size_t)height;
    size_t rgb_size = pixel_count * 3U;
    uint8_t *rgb = malloc(rgb_size);
    MbPixel *source_pixels = malloc(pixel_count * sizeof(*source_pixels));
    MbPixel *recon_pixels = malloc(pixel_count * sizeof(*recon_pixels));
    MbPixel *previous_pixels = malloc(pixel_count * sizeof(*previous_pixels));
    MbPixel *decoded_pixels = malloc(pixel_count * sizeof(*decoded_pixels));
    MbEncodeWorkspace workspace = { 0U, 0U, 0U, 0U, NULL, NULL };
    int have_workspace = 0;
    unsigned current_loss = loss_level;
    ReplayBuffer payload;
    FILE *input = NULL;
    size_t frame_number = 0U;
    int result = EXIT_FAILURE;

    if (keys_prefix != NULL && !codec->supports_keys) {
        fprintf(stderr, "type %u key frames are not supported (--keys)\n",
                codec->number);
        return EXIT_FAILURE;
    }
    if (codec->rgb_only && input_format != INPUT_RGB24) {
        fprintf(stderr, "type %u encode supports only RGB24 input\n",
                codec->number);
        return EXIT_FAILURE;
    }
    replay_buffer_init(&payload);
    if (rgb == NULL || source_pixels == NULL || recon_pixels == NULL ||
        previous_pixels == NULL || decoded_pixels == NULL) {
        fprintf(stderr, "unable to allocate frame buffers\n");
        goto cleanup;
    }
    /* The temporal-search cache only earns its keep across rate-control
       retries, so it is allocated only when a target byte budget is set. */
    if (target_bytes != 0U) {
        if (mb_encode_workspace_init(&workspace, width, height) != REPLAY_OK) {
            fprintf(stderr, "unable to allocate encoder search workspace\n");
            goto cleanup;
        }
        have_workspace = 1;
    }
    input = strcmp(input_path, "-") == 0 ? stdin : fopen(input_path, "rb");
    if (input == NULL) {
        perror(input_path);
        goto cleanup;
    }

    for (;;) {
        FrameReadResult read_result =
            read_frame(input, input_path, rgb, rgb_size);
        MbFrame source = { width, height, width, source_pixels };
        MbFrame reconstructed = { width, height, width, recon_pixels };
        MbFrame previous = { width, height, width, previous_pixels };
        MbFrame decoded = { width, height, width, decoded_pixels };
        const MbFrame *previous_arg = frame_number == 0U ? NULL : &previous;
        MbEncodeOptions options;
        MbToolStats stats;
        unsigned retry = 0U;
        ReplayStatus status;

        if (read_result == FRAME_READ_EOF) {
            break;
        }
        if (read_result != FRAME_READ_OK) {
            goto cleanup;
        }
        status = input_format == INPUT_RGB24
                     ? codec->from_rgb24(rgb, (size_t)width * 3U, &source,
                                         dither)
                     : codec->unpack(rgb, &source);
        if (status != REPLAY_OK) {
            fprintf(stderr, "frame %zu: input conversion failed: %s\n",
                    frame_number, replay_status_string(status));
            goto cleanup;
        }

        options.allow_stationary = !data_only && previous_arg != NULL;
        options.allow_temporal = !data_only && previous_arg != NULL;
        options.allow_spatial = !data_only;
        options.allow_split = !data_only;
        options.policy = policy;

        /*
         * Device-bandwidth rate control: retry the frame at successively looser
         * quality rows until it fits the target byte window. The key frame is
         * encoded at the requested level; the carried level seeds the next
         * frame. The workspace cache makes the retries cheap.
         */
        {
            int rate_enabled = target_bytes != 0U && previous_arg != NULL;
            MbRateControl rate_control;

            if (rate_enabled &&
                mb_rate_control_init(&rate_control, target_bytes,
                                     current_loss) != REPLAY_OK) {
                fprintf(stderr, "frame %zu: invalid rate-control target\n",
                        frame_number);
                goto cleanup;
            }
            if (have_workspace) {
                mb_encode_workspace_reset(&workspace);
            }
            options.workspace = rate_enabled ? &workspace : NULL;
            for (;;) {
                options.loss_level =
                    rate_enabled ? rate_control.loss_level : current_loss;
                status = codec->encode(&source, previous_arg, &options, variant,
                                       &payload, &reconstructed, &stats);
                if (status != REPLAY_OK) {
                    fprintf(stderr, "frame %zu: encode failed: %s\n",
                            frame_number, replay_status_string(status));
                    goto cleanup;
                }
                if (!rate_enabled ||
                    mb_rate_control_observe(&rate_control, payload.size) ==
                        MB_RATE_ACCEPT) {
                    break;
                }
                ++retry;
            }
            current_loss = options.loss_level;
        }
        /* Independent decode confirms the stream matches the reconstruction. */
        if (self_check_frames) {
            status = codec->verify(payload.data, payload.size, previous_arg,
                                   &decoded, variant);
            if (status != REPLAY_OK ||
                memcmp(decoded_pixels, recon_pixels,
                       pixel_count * sizeof(*recon_pixels)) != 0) {
                fprintf(stderr, "frame %zu: decode self-check failed\n",
                        frame_number);
                goto cleanup;
            }
        }

        printf("frame=%zu codec=%u name=\"%s\" ", frame_number, codec->number,
               codec->name);
        if (codec->number == 20U) {
            printf("variant=%s ",
                   variant == CODEC_MOVINGBLOCKSBETA_NEW ? "new" : "old");
        }
        printf("retry=%u loss_level=%u bits=%zu bytes=%zu data4x4=%zu "
               "stationary4x4=%zu temporal4x4=%zu spatial4x4=%zu split4x4=%zu "
               "data2x2=%zu stationary2x2=%zu temporal2x2=%zu spatial2x2=%zu "
               "verify=ok\n",
               retry, options.loss_level, stats.bits_written, payload.size,
               stats.data4x4_blocks, stats.stationary4x4_blocks,
               stats.temporal4x4_blocks, stats.spatial4x4_blocks,
               stats.split4x4_blocks, stats.data2x2_blocks,
               stats.stationary2x2_blocks, stats.temporal2x2_blocks,
               stats.spatial2x2_blocks);

        if (payload_path != NULL || payload_prefix != NULL) {
            char generated[1024];
            const char *frame_payload = payload_path;

            if (payload_prefix != NULL) {
                if (make_frame_path(generated, sizeof(generated),
                                    payload_prefix, frame_number,
                                    codec->payload_ext) != EXIT_SUCCESS) {
                    goto cleanup;
                }
                frame_payload = generated;
            }
            if (write_payload(frame_payload, &payload) != EXIT_SUCCESS) {
                goto cleanup;
            }
        }
        if (sink != NULL &&
            frame_sink_add(sink, &payload, &reconstructed) != EXIT_SUCCESS) {
            goto cleanup;
        }
        if (recon_ppm_path != NULL || recon_prefix != NULL) {
            char generated[1024];
            const char *frame_ppm = recon_ppm_path;

            if (recon_prefix != NULL) {
                if (make_frame_path(generated, sizeof(generated), recon_prefix,
                                    frame_number, ".ppm") != EXIT_SUCCESS) {
                    goto cleanup;
                }
                frame_ppm = generated;
            }
            if (codec->write_ppm(frame_ppm, &reconstructed, rgb,
                                 (size_t)width * 3U) != EXIT_SUCCESS) {
                goto cleanup;
            }
        }
        if (keys_prefix != NULL) {
            char generated[1024];

            if (make_frame_path(generated, sizeof(generated), keys_prefix,
                                frame_number, ".key") != EXIT_SUCCESS ||
                codec->write_key(generated, &reconstructed) != EXIT_SUCCESS) {
                goto cleanup;
            }
        }

        memcpy(previous_pixels, recon_pixels,
               pixel_count * sizeof(*recon_pixels));
        ++frame_number;
        if (frame_limit != 0U && frame_number >= frame_limit) {
            break;
        }
    }
    if (write_pad_frames(codec->number, width, height, payload_prefix,
                         codec->payload_ext, keys_prefix, pad_to_multiple,
                         &frame_number, sink) != EXIT_SUCCESS) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (input != NULL && input != stdin) {
        fclose(input);
    }
    if (have_workspace) {
        mb_encode_workspace_destroy(&workspace);
    }
    free(decoded_pixels);
    free(previous_pixels);
    free(recon_pixels);
    free(source_pixels);
    free(rgb);
    replay_buffer_free(&payload);
    return result;
}

static int read_file_buffer(const char *path, ReplayBuffer *out)
{
    FILE *file = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    uint8_t block[65536];

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(block, 1U, sizeof(block), file);

        if (count != 0U && replay_buffer_append(out, block, count) != REPLAY_OK) {
            fprintf(stderr, "%s: out of memory\n", path);
            if (file != stdin) {
                fclose(file);
            }
            return EXIT_FAILURE;
        }
        if (count != sizeof(block)) {
            if (ferror(file)) {
                perror(path);
                if (file != stdin) {
                    fclose(file);
                }
                return EXIT_FAILURE;
            }
            break;
        }
    }
    if (file != stdin && fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*
 * Options for assembling the collected frames (and optional audio/poster) into a
 * complete AE7 movie -- the direct-to-container path. Mirrors the subset of
 * replay-join's interface the encoder drives. `container_type` is the
 * compression number written in the movie header, which may differ from the
 * encoding codec (e.g. type 20 "new" aliased to a free Decomp number).
 */
typedef struct {
    unsigned codec;            /* encoding codec, for key packing */
    unsigned container_type;   /* compression number in the header */
    unsigned width;
    unsigned height;
    unsigned pixel_depth;
    const char *pixel_label;
    double fps;
    unsigned frames_per_chunk;
    unsigned align_mask;
    int want_keys;
    const char *audio_input_path;
    const char *sound_encode;
    unsigned sound_rate;
    unsigned sound_channels;
    const char *poster_path;
    unsigned poster_w;
    unsigned poster_h;
    int no_poster;
    const char *title;
    const char *copyright;
    const char *author;
    const char *output_path;
} ContainerOptions;

static int assemble_container(const FrameSink *sink, const ContainerOptions *c)
{
    ReplayAe7WriteOptions options;
    ReplayAe7WriteTrack track;
    ReplayBuffer sound_encoded;
    ReplayBuffer pcm;
    ReplayBuffer poster_pixels;
    ReplayBuffer sprite;
    ReplayBuffer movie;
    const uint8_t **frame_ptrs = NULL;
    size_t *frame_sizes = NULL;
    const uint8_t **key_ptrs = NULL;
    char error[256];
    int result = EXIT_FAILURE;
    size_t i;

    memset(&options, 0, sizeof(options));
    memset(&track, 0, sizeof(track));
    replay_buffer_init(&sound_encoded);
    replay_buffer_init(&pcm);
    replay_buffer_init(&poster_pixels);
    replay_buffer_init(&sprite);
    replay_buffer_init(&movie);

    if (sink->count == 0U) {
        fprintf(stderr, "%s: no frames encoded\n", c->output_path);
        goto done;
    }
    frame_ptrs = calloc(sink->count, sizeof(*frame_ptrs));
    frame_sizes = calloc(sink->count, sizeof(*frame_sizes));
    if (frame_ptrs == NULL || frame_sizes == NULL) {
        fprintf(stderr, "unable to allocate frame tables\n");
        goto done;
    }
    for (i = 0U; i < sink->count; ++i) {
        frame_ptrs[i] = sink->payloads[i].data;
        frame_sizes[i] = sink->payloads[i].size;
    }

    options.title = c->title;
    options.copyright = c->copyright;
    options.author = c->author;
    options.video_codec = c->container_type;
    options.width = c->width;
    options.height = c->height;
    options.pixel_depth = c->pixel_depth;
    options.pixel_label = c->pixel_label;
    options.frames_per_second = c->fps;
    options.frame_data = frame_ptrs;
    options.frame_size = frame_sizes;
    options.frame_count = sink->count;
    options.frames_per_chunk = c->frames_per_chunk;
    options.chunk_seconds = 1.0;
    options.align_mask = c->align_mask;

    if (c->want_keys && sink->want_keys) {
        key_ptrs = calloc(sink->count, sizeof(*key_ptrs));
        if (key_ptrs == NULL) {
            fprintf(stderr, "unable to allocate key tables\n");
            goto done;
        }
        for (i = 0U; i < sink->count; ++i) {
            key_ptrs[i] = sink->keys[i].data;
        }
        options.write_keys = 1;
        options.key_data = key_ptrs;
        options.key_size = (size_t)c->width * c->height * 2U;
    }

    if (c->audio_input_path != NULL) {
        if (read_file_buffer(c->audio_input_path, &pcm) != EXIT_SUCCESS) {
            goto done;
        }
        if (replay_build_pcm_track(pcm.data, pcm.size, c->sound_encode,
                                   c->sound_rate, c->sound_channels,
                                   &sound_encoded, &track, error,
                                   sizeof(error)) != REPLAY_OK) {
            fprintf(stderr, "%s\n", error);
            goto done;
        }
        options.tracks = &track;
        options.track_count = 1U;
    }

    if (c->poster_path != NULL) {
        unsigned pw = c->poster_w != 0U ? c->poster_w : c->width;
        unsigned ph = c->poster_h != 0U ? c->poster_h : c->height;
        size_t want = (size_t)pw * ph * 2U;

        if (read_file_buffer(c->poster_path, &poster_pixels) != EXIT_SUCCESS) {
            goto done;
        }
        if (poster_pixels.size != want) {
            fprintf(stderr,
                    "%s: poster must be %ux%u bgr555 pixels (%zu bytes), got %zu\n",
                    c->poster_path, pw, ph, want, poster_pixels.size);
            goto done;
        }
        if (replay_build_poster(poster_pixels.data, pw, ph, &sprite) !=
            REPLAY_OK) {
            fprintf(stderr, "unable to build poster sprite\n");
            goto done;
        }
        options.sprite_data = sprite.data;
        options.sprite_size = sprite.size;
    } else if (!c->no_poster) {
        options.sprite_data = replay_default_poster;
        options.sprite_size = replay_default_poster_size;
    }

    if (replay_ae7_write(&options, &movie, error, sizeof(error)) != REPLAY_OK) {
        fprintf(stderr, "%s: %s\n", c->output_path, error);
        goto done;
    }
    {
        FILE *out = fopen(c->output_path, "wb");

        if (out == NULL) {
            perror(c->output_path);
            goto done;
        }
        if (fwrite(movie.data, 1U, movie.size, out) != movie.size ||
            fclose(out) != 0) {
            perror(c->output_path);
            goto done;
        }
    }
    printf("codec=%u container=%u size=%ux%u frames=%zu fps=%g bytes=%zu "
           "output=\"%s\"\n",
           c->codec, c->container_type, c->width, c->height, sink->count,
           c->fps, movie.size, c->output_path);
    result = EXIT_SUCCESS;

done:
    free(frame_ptrs);
    free(frame_sizes);
    free(key_ptrs);
    replay_buffer_free(&sound_encoded);
    replay_buffer_free(&pcm);
    replay_buffer_free(&poster_pixels);
    replay_buffer_free(&sprite);
    replay_buffer_free(&movie);
    return result;
}

/*
 * Pad the collected frames so the writer produces only uniform, fully-populated
 * chunks. The player decodes a fixed "frames per chunk" from every chunk, so any
 * chunk holding fewer frames than that nominal is decoded past its data into
 * garbage. *eff_fpc returns the frames-per-chunk to hand the writer.
 *
 * Two cases:
 *  - A movie that spans more than one chunk is padded up to a whole multiple of
 *    frames-per-chunk (the final chunk becomes full).
 *  - A movie that fits in a single chunk but carries audio is split by the
 *    writer into two chunks (it forces >=2 chunks so the sound prefetch has a
 *    real next chunk). Halving an odd count yields two uneven chunks, so pad to
 *    two EQUAL chunks of ceil(real/2) -- at most one extra frame.
 */
static int pad_sink_for_chunks(FrameSink *sink, unsigned codec, unsigned width,
                               unsigned height, unsigned fpc, int have_audio,
                               unsigned *eff_fpc)
{
    size_t real = sink->count;
    size_t target;
    unsigned chunk;
    ReplayBuffer payload;
    int result = EXIT_FAILURE;

    if (fpc == 0U) {
        fpc = real != 0U ? (unsigned)real : 1U;
    }
    if (real == 0U) {
        *eff_fpc = fpc;
        return EXIT_SUCCESS;
    }
    if (have_audio && real <= (size_t)fpc) {
        chunk = (unsigned)((real + 1U) / 2U); /* ceil(real / 2) */
        if (chunk < 3U) {
            chunk = 3U; /* the player needs frames-per-chunk >= 3 (player-bugs.md) */
        }
        target = (size_t)chunk * 2U;
    } else {
        chunk = fpc;
        target = ((real + (size_t)fpc - 1U) / (size_t)fpc) * (size_t)fpc;
    }
    *eff_fpc = chunk;
    if (target <= real) {
        return EXIT_SUCCESS;
    }

    replay_buffer_init(&payload);
    if (mb_repeat_payload(codec, width, height, &payload) != REPLAY_OK) {
        fprintf(stderr, "cannot build repeat frame for codec %u\n", codec);
        goto done;
    }
    while (sink->count < target) {
        if (frame_sink_add(sink, &payload, NULL) != EXIT_SUCCESS) {
            goto done;
        }
    }
    result = EXIT_SUCCESS;
done:
    replay_buffer_free(&payload);
    return result;
}

/* ------------------------------------------------------------------------- *
 * Type 1, Moving Lines.
 *
 * Moving Lines works on opaque 15-bit pixels rather than the Moving Blocks
 * MbFrame, so it gets its own encode loop instead of the shared run_mb_encode
 * driver. RGB24 input is packed as RGB555 (one of the format's two advertised
 * colour models). Each frame is encoded against the previous reconstruction,
 * round-trip self-checked, and emitted as a `.mln` payload and/or a PPM preview.
 * ------------------------------------------------------------------------- */

static uint16_t rgb24_to_rgb555(const uint8_t *rgb)
{
    unsigned r = rgb[0] >> 3, g = rgb[1] >> 3, b = rgb[2] >> 3;

    /* RISC OS 15-bit convention: red in the low bits (ffmpeg bgr555le), the
       same packing as the Replay poster (see docs/player-bugs.md). */
    return (uint16_t)((b << 10) | (g << 5) | r);
}

static int write_movinglines_ppm(const char *path, const uint16_t *pixels,
                                 unsigned width, unsigned height)
{
    FILE *file = fopen(path, "wb");
    size_t count = (size_t)width * height;
    size_t i;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (i = 0; i < count; ++i) {
        uint8_t rgb[3] = {
            (uint8_t)((pixels[i] & 0x1FU) << 3),         /* red in the low bits */
            (uint8_t)(((pixels[i] >> 5) & 0x1FU) << 3),
            (uint8_t)(((pixels[i] >> 10) & 0x1FU) << 3)
        };

        if (fwrite(rgb, 1, 3, file) != 3) {
            fclose(file);
            return EXIT_FAILURE;
        }
    }
    return fclose(file) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_movinglines(const char *input_path, InputFormat input_format,
                           unsigned width, unsigned height,
                           const char *payload_path,
                           const char *payload_prefix, size_t frame_limit,
                           const char *recon_prefix,
                           const char *recon_ppm_path, unsigned pad_to_multiple,
                           int yuv, MbColorDither dither, FrameSink *sink)
{
    size_t pixel_count = (size_t)width * height;
    size_t rgb_size = pixel_count * 3U;
    uint8_t *rgb = malloc(rgb_size);
    uint16_t *source = malloc(pixel_count * sizeof(*source));
    uint16_t *previous = malloc(pixel_count * sizeof(*previous));
    uint16_t *decoded = malloc(pixel_count * sizeof(*decoded));
    MbPixel *yuv_pixels = yuv ? malloc(pixel_count * sizeof(*yuv_pixels)) : NULL;
    ReplayBuffer payload;
    FILE *input = NULL;
    size_t frame_number = 0U;
    int result = EXIT_FAILURE;

    replay_buffer_init(&payload);
    if (input_format != INPUT_RGB24) {
        fprintf(stderr, "type 1 encode supports only RGB24 input\n");
        goto cleanup;
    }
    if (rgb == NULL || source == NULL || previous == NULL || decoded == NULL ||
        (yuv && yuv_pixels == NULL)) {
        fprintf(stderr, "unable to allocate frame buffers\n");
        goto cleanup;
    }
    input = strcmp(input_path, "-") == 0 ? stdin : fopen(input_path, "rb");
    if (input == NULL) {
        perror(input_path);
        goto cleanup;
    }

    for (;;) {
        FrameReadResult read_result =
            read_frame(input, input_path, rgb, rgb_size);
        const uint16_t *previous_arg = frame_number == 0U ? NULL : previous;
        size_t i;

        if (read_result == FRAME_READ_EOF) {
            break;
        }
        if (read_result != FRAME_READ_OK) {
            goto cleanup;
        }
        if (yuv) {
            /* RGB24 -> YUV555 (5-bit Y, signed 5-bit U/V), packed Y|U<<5|V<<10
               -- the native type-7/17 YUV555 word layout. */
            MbFrame frame = { width, height, width, yuv_pixels };

            if (mb_color_rgb24_to_yuv555(rgb, (size_t)width * 3U, &frame,
                                         dither) != REPLAY_OK) {
                fprintf(stderr, "frame %zu: YUV conversion failed\n",
                        frame_number);
                goto cleanup;
            }
            for (i = 0; i < pixel_count; ++i) {
                source[i] = (uint16_t)(yuv_pixels[i].y |
                                       (yuv_pixels[i].u << 5) |
                                       (yuv_pixels[i].v << 10));
            }
        } else {
            for (i = 0; i < pixel_count; ++i) {
                source[i] = rgb24_to_rgb555(&rgb[3 * i]);
            }
        }
        if (codec_movinglines_encode_frame(source, previous_arg, width, height,
                                            &payload) != REPLAY_OK) {
            fprintf(stderr, "frame %zu: encode failed\n", frame_number);
            goto cleanup;
        }
        /* Independent decode confirms the stream matches the reconstruction. */
        if (codec_movinglines_decode_frame(payload.data, payload.size,
                                            previous_arg, decoded, width, height,
                                            NULL) != REPLAY_OK ||
            memcmp(decoded, source, pixel_count * sizeof(*source)) != 0) {
            fprintf(stderr, "frame %zu: decode self-check failed\n",
                    frame_number);
            goto cleanup;
        }

        if (payload_path != NULL || payload_prefix != NULL) {
            char generated[1024];
            const char *frame_payload = payload_path;

            if (payload_prefix != NULL) {
                if (make_frame_path(generated, sizeof(generated),
                                    payload_prefix, frame_number, ".mln") !=
                    EXIT_SUCCESS) {
                    goto cleanup;
                }
                frame_payload = generated;
            }
            if (write_payload(frame_payload, &payload) != EXIT_SUCCESS) {
                goto cleanup;
            }
        }
        if (recon_ppm_path != NULL || recon_prefix != NULL) {
            char generated[1024];
            const char *frame_ppm = recon_ppm_path;

            if (recon_prefix != NULL) {
                if (make_frame_path(generated, sizeof(generated), recon_prefix,
                                    frame_number, ".ppm") != EXIT_SUCCESS) {
                    goto cleanup;
                }
                frame_ppm = generated;
            }
            if (write_movinglines_ppm(frame_ppm, decoded, width, height) !=
                EXIT_SUCCESS) {
                goto cleanup;
            }
        }
        if (sink != NULL &&
            frame_sink_add(sink, &payload, NULL) != EXIT_SUCCESS) {
            goto cleanup;
        }
        printf("frame=%zu codec=1 name=\"Moving Lines\" bytes=%zu verify=ok\n",
               frame_number, payload.size);

        memcpy(previous, decoded, pixel_count * sizeof(*previous));
        ++frame_number;
        if (frame_limit != 0U && frame_number >= frame_limit) {
            break;
        }
    }
    if (frame_number == 0U) {
        fprintf(stderr, "%s: no complete input frames\n", input_path);
        goto cleanup;
    }
    /* Loose-mode chunk padding; the direct-to-container path pads the sink at
       assembly instead (pad_to_multiple is 0 there). */
    if (write_pad_frames(1U, width, height, payload_prefix, ".mln", NULL,
                         pad_to_multiple, &frame_number, sink) != EXIT_SUCCESS) {
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (input != NULL && input != stdin) {
        fclose(input);
    }
    replay_buffer_free(&payload);
    free(yuv_pixels);
    free(decoded);
    free(previous);
    free(source);
    free(rgb);
    return result;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *payload_path = NULL;
    const char *payload_prefix = NULL;
    const char *trace_path = NULL;
    const char *recon_ppm_path = NULL;
    const char *recon_prefix = NULL;
    const char *keys_prefix = NULL;
    unsigned width = 0U;
    unsigned height = 0U;
    unsigned codec = 0U;
    unsigned loss_level = 0U;
    unsigned current_loss_level;
    size_t target_bytes = 0U;
    size_t frame_limit = 0U;
    InputFormat input_format = INPUT_RGB24;
    MbColorDither dither = MB_COLOR_DITHER_NONE;
    int ml_yuv = 0; /* codec 1: pack YUV555 instead of RGB555 */
    CodecMovingBlocksBetaVariant beta_variant = CODEC_MOVINGBLOCKSBETA_OLD;
    unsigned pad_to_multiple = 0U;
    unsigned encode_pad = 0U;
    int pad_chunks = 0;
    /* Direct-to-container (--output) options, mirroring replay-join. */
    ContainerOptions container;
    FrameSink sink;
    FrameSink *sinkp = NULL;
    int want_keys = 0;
    CodecSuperMovingBlocksPolicy policy =
        CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR;
    RateSearch rate_search = RATE_SEARCH_LINEAR;
    int data_only = 0;
    uint8_t *rgb = NULL;
    MbPixel *source_pixels = NULL;
    MbPixel *reconstructed_pixels = NULL;
    MbPixel *previous_pixels = NULL;
    MbPixel *decoded_pixels = NULL;
    ReplayBuffer payload;
    CodecSuperMovingBlocksWorkspace workspace = { NULL };
    int have_workspace = 0;
    FILE *input = NULL;
    FILE *trace = NULL;
    size_t pixel_count;
    size_t rgb_size;
    size_t frame_number = 0U;
    int result = EXIT_FAILURE;
    int i;

    memset(&container, 0, sizeof(container));
    container.pixel_depth = 16U;
    container.sound_encode = "vidc-e8";
    container.sound_rate = 11025U;
    container.sound_channels = 1U;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value > 999UL) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            codec = (unsigned)value;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--input-format") == 0 &&
                   i + 1 < argc) {
            const char *format = argv[++i];

            if (strcmp(format, "rgb24") == 0) {
                input_format = INPUT_RGB24;
            } else if (strcmp(format, "6y5uv") == 0) {
                input_format = INPUT_6Y5UV;
            } else if (strcmp(format, "yuv555") == 0) {
                input_format = INPUT_YUV555;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--dither") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];

            if (strcmp(mode, "4x4") == 0) {
                dither = MB_COLOR_DITHER_ORDERED_4X4;
            } else if (strcmp(mode, "8x8") == 0) {
                dither = MB_COLOR_DITHER_ORDERED_8X8;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            const char *name = argv[++i];

            if (strcmp(name, "old") == 0) {
                beta_variant = CODEC_MOVINGBLOCKSBETA_OLD;
            } else if (strcmp(name, "new") == 0) {
                beta_variant = CODEC_MOVINGBLOCKSBETA_NEW;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--no-dither") == 0) {
            dither = MB_COLOR_DITHER_NONE;
        } else if (strcmp(argv[i], "--colour") == 0 && i + 1 < argc) {
            const char *name = argv[++i];

            if (strcmp(name, "rgb") == 0) {
                ml_yuv = 0;
            } else if (strcmp(name, "yuv") == 0) {
                ml_yuv = 1;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            payload_path = argv[++i];
        } else if (strcmp(argv[i], "--payload-prefix") == 0 && i + 1 < argc) {
            payload_prefix = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--recon-ppm") == 0 && i + 1 < argc) {
            recon_ppm_path = argv[++i];
        } else if (strcmp(argv[i], "--recon-prefix") == 0 && i + 1 < argc) {
            recon_prefix = argv[++i];
        } else if (strcmp(argv[i], "--keys-prefix") == 0 && i + 1 < argc) {
            keys_prefix = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value == 0UL || value > SIZE_MAX) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            frame_limit = (size_t)value;
        } else if (strcmp(argv[i], "--data-only") == 0) {
            data_only = 1;
        } else if (strcmp(argv[i], "--verify") == 0) {
            self_check_frames = 1;
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            self_check_frames = 0;
        } else if (strcmp(argv[i], "--loss-level") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value > 28UL) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            loss_level = (unsigned)value;
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            const char *name = argv[++i];

            if (strcmp(name, "ordered") == 0) {
                policy = CODEC_SUPERMOVINGBLOCKS_POLICY_ORDERED;
            } else if (strcmp(name, "lowest-error") == 0) {
                policy = CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--rate-search") == 0 && i + 1 < argc) {
            const char *name = argv[++i];

            if (strcmp(name, "linear") == 0) {
                rate_search = RATE_SEARCH_LINEAR;
            } else if (strcmp(name, "bracketed") == 0) {
                rate_search = RATE_SEARCH_BRACKETED;
            } else {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--target-bytes") == 0 && i + 1 < argc) {
            char *end;
            unsigned long long value = strtoull(argv[++i], &end, 10);
            if (*end != '\0' || value == 0ULL || value > SIZE_MAX) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            target_bytes = (size_t)value;
        } else if (strcmp(argv[i], "--pad-to-multiple") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value > 100000UL) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            pad_to_multiple = (unsigned)value;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            container.output_path = argv[++i];
        } else if (strcmp(argv[i], "--audio-input") == 0 && i + 1 < argc) {
            container.audio_input_path = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            container.fps = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--frames-per-chunk") == 0 && i + 1 < argc) {
            container.frames_per_chunk = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--container-type") == 0 && i + 1 < argc) {
            container.container_type = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--pixel-label") == 0 && i + 1 < argc) {
            container.pixel_label = argv[++i];
        } else if (strcmp(argv[i], "--pixel-depth") == 0 && i + 1 < argc) {
            container.pixel_depth = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--align") == 0 && i + 1 < argc) {
            container.align_mask = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--keys") == 0) {
            want_keys = 1;
        } else if (strcmp(argv[i], "--sound-encode") == 0 && i + 1 < argc) {
            container.sound_encode = argv[++i];
        } else if (strcmp(argv[i], "--sound-rate") == 0 && i + 1 < argc) {
            container.sound_rate = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sound-channels") == 0 && i + 1 < argc) {
            container.sound_channels = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--poster") == 0 && i + 1 < argc) {
            container.poster_path = argv[++i];
        } else if (strcmp(argv[i], "--poster-size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &container.poster_w,
                       &container.poster_h) != 2) {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--no-poster") == 0) {
            container.no_poster = 1;
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            container.title = argv[++i];
        } else if (strcmp(argv[i], "--copyright") == 0 && i + 1 < argc) {
            container.copyright = argv[++i];
        } else if (strcmp(argv[i], "--author") == 0 && i + 1 < argc) {
            container.author = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            char tail;
            if (sscanf(argv[++i], "%ux%u%c", &width, &height, &tail) != 2) {
                usage(stderr);
                return EXIT_FAILURE;
            }
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    /*
     * Direct-to-container mode: collect frames in memory and write a complete
     * movie. Incompatible with the single-frame --payload sink; --payload-prefix
     * may still be given to additionally dump per-frame payloads for debugging.
     */
    if (container.output_path != NULL) {
        if (payload_path != NULL || !(container.fps > 0.0)) {
            fprintf(stderr,
                    "--output needs --fps and is incompatible with --payload\n");
            return EXIT_FAILURE;
        }
        if (want_keys && (codec == 20U || codec == 1U)) {
            fprintf(stderr, "type %u key frames are not supported (--keys)\n",
                    codec);
            return EXIT_FAILURE;
        }
        container.codec = codec;
        container.container_type =
            container.container_type != 0U ? container.container_type : codec;
        container.width = width;
        container.height = height;
        container.want_keys = want_keys;
        /* Moving Lines carries no colour in the stream; the player builds the
           colour-map name from the bracketed pixel label plus the depth --
           ColourMap.<label><depth>, e.g. RGB16 / YUV16 (bas/Player line 7450) --
           so the label must be the bare colour-map prefix (RGB / YUV), not a
           "5,5,5"-style description. Set it to the model the muxer packed unless
           one was given explicitly. */
        if (codec == 1U && container.pixel_label == NULL) {
            container.pixel_label = ml_yuv ? "YUV" : "RGB";
        }
        frame_sink_init(&sink, codec, want_keys);
        sinkp = &sink;
        /* The direct path pads after encoding, once the real frame count and
         * audio presence are known, so the writer gets only uniform full chunks
         * (the per-frame padding done during encode cannot see either). */
        pad_chunks = pad_to_multiple != 0;
        encode_pad = 0U;
    } else {
        encode_pad = pad_to_multiple;
    }
    if (codec == 1U) {
        if (input_path == NULL || width == 0U || height == 0U ||
            (size_t)width > SIZE_MAX / (size_t)height ||
            data_only || target_bytes != 0U || keys_prefix != NULL ||
            want_keys ||
            (recon_ppm_path != NULL && recon_prefix != NULL) ||
            (sinkp == NULL && payload_path == NULL && payload_prefix == NULL &&
             recon_prefix == NULL && recon_ppm_path == NULL) ||
            (payload_path != NULL &&
             (payload_prefix != NULL || frame_limit > 1U ||
              recon_prefix != NULL))) {
            usage(stderr);
            result = EXIT_FAILURE;
            goto assemble;
        }
        result = run_movinglines(input_path, input_format, width, height,
                                 payload_path, payload_prefix, frame_limit,
                                 recon_prefix, recon_ppm_path, encode_pad,
                                 ml_yuv, dither, sinkp);
        goto assemble;
    }
    if (codec == 7U || codec == 17U || codec == 20U) {
        MbEncodePolicy mb_policy =
            policy == CODEC_SUPERMOVINGBLOCKS_POLICY_LOWEST_ERROR
                ? MB_ENCODE_POLICY_LOWEST_ERROR
                : MB_ENCODE_POLICY_ORDERED;
        const MbToolCodec *tool_codec = codec == 7U    ? &mb_tool_codec7
                                        : codec == 20U ? &mb_tool_codec20
                                                       : &mb_tool_codec17;

        if (input_path == NULL ||
            (sinkp == NULL &&
             (payload_path == NULL) == (payload_prefix == NULL)) ||
            width == 0U || height == 0U ||
            (width % 4U) != 0U || (height % 4U) != 0U ||
            loss_level >= MB_QUALITY_LEVEL_COUNT ||
            (recon_ppm_path != NULL && recon_prefix != NULL) ||
            (payload_path != NULL &&
             (frame_limit > 1U || recon_prefix != NULL ||
              keys_prefix != NULL))) {
            usage(stderr);
            return EXIT_FAILURE;
        }
        result = run_mb_encode(
            tool_codec, (int)beta_variant, input_path, input_format, width,
            height, payload_path, payload_prefix, frame_limit, loss_level,
            data_only, recon_prefix, recon_ppm_path, keys_prefix, target_bytes,
            dither, mb_policy, encode_pad, sinkp);
        goto assemble;
    }
    if (codec != 19U || input_path == NULL ||
        (sinkp == NULL && (payload_path == NULL) == (payload_prefix == NULL)) ||
        (recon_ppm_path != NULL && recon_prefix != NULL) ||
        (payload_path != NULL &&
         (frame_limit > 1U || recon_prefix != NULL || keys_prefix != NULL)) ||
        (payload_prefix != NULL && recon_ppm_path != NULL) ||
        (recon_ppm_path != NULL && keys_prefix != NULL) ||
        (data_only && target_bytes != 0U) || width == 0U ||
        height == 0U || (width & 3U) != 0U || (height & 3U) != 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    pixel_count = (size_t)width * height;
    if (pixel_count > SIZE_MAX / 3U ||
        pixel_count > SIZE_MAX / sizeof(*source_pixels)) {
        fprintf(stderr, "frame dimensions are too large\n");
        return EXIT_FAILURE;
    }
    rgb_size = pixel_count * 3U;
    rgb = malloc(rgb_size);
    source_pixels = malloc(pixel_count * sizeof(*source_pixels));
    reconstructed_pixels = malloc(pixel_count * sizeof(*reconstructed_pixels));
    previous_pixels = malloc(pixel_count * sizeof(*previous_pixels));
    decoded_pixels = malloc(pixel_count * sizeof(*decoded_pixels));
    if (rgb == NULL || source_pixels == NULL || reconstructed_pixels == NULL ||
        previous_pixels == NULL || decoded_pixels == NULL) {
        fprintf(stderr, "unable to allocate frame buffers\n");
        goto done;
    }
    input = strcmp(input_path, "-") == 0 ? stdin : fopen(input_path, "rb");
    if (input == NULL) {
        perror(input_path);
        goto done;
    }
    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL) {
            perror(trace_path);
            goto done;
        }
    }
    replay_buffer_init(&payload);
    /* The temporal-search cache only earns its keep across rate-control
       retries, so it is allocated only when a target byte budget is set. */
    if (target_bytes != 0U) {
        if (codec_supermovingblocks_workspace_init(
                &workspace, width, height) != REPLAY_OK) {
            fprintf(stderr, "unable to allocate encoder search workspace\n");
            goto free_payload;
        }
        have_workspace = 1;
    }
    current_loss_level = loss_level;

    /*
     * The previous buffer always contains decoder-visible reconstruction, not
     * source pixels. Referencing source pixels here would cause encoder and
     * player state to diverge after the first chroma-averaged data block.
     */
    for (;;) {
        FrameReadResult read_result = read_frame(input, input_path, rgb, rgb_size);
        MbFrame source = { width, height, width, source_pixels };
        MbFrame reconstructed =
            { width, height, width, reconstructed_pixels };
        MbFrame previous = { width, height, width, previous_pixels };
        MbFrame decoded = { width, height, width, decoded_pixels };
        CodecSuperMovingBlocksEncodeOptions options = {
            !data_only && frame_number != 0U,
            !data_only && frame_number != 0U,
            !data_only,
            !data_only,
            current_loss_level,
            policy,
            &workspace
        };
        CodecSuperMovingBlocksEncodeStats stats;
        const MbFrame *previous_arg = frame_number == 0U ? NULL : &previous;
        char generated_payload[4096];
        char generated_ppm[4096];
        char generated_key[4096];
        const char *frame_payload = payload_path;
        const char *frame_ppm = recon_ppm_path;
        size_t consumed_bits;
        MbRateControl rate_control;
        int rate_control_enabled = target_bytes != 0U && frame_number != 0U;
        unsigned retry = 0U;
        BracketedRateSearch bracketed = { 0, 0U, 0U, 0U, 0U, 0, 0 };
        ReplayStatus status;

        if (read_result == FRAME_READ_EOF) {
            break;
        }
        if (read_result == FRAME_READ_ERROR) {
            goto free_payload;
        }
        if (have_workspace) {
            codec_supermovingblocks_workspace_reset(&workspace);
        }
        status = input_format == INPUT_RGB24
                     ? mb_color_rgb24_to_6y5uv(
                           rgb, (size_t)width * 3U, &source, dither)
                     : unpack_6y5uv(rgb, &source);
        if (status != REPLAY_OK) {
            fprintf(stderr, "input conversion failed: %s\n",
                    replay_status_string(status));
            goto free_payload;
        }
        if (target_bytes != 0U &&
            mb_rate_control_init(&rate_control, target_bytes,
                                 current_loss_level) != REPLAY_OK) {
            fprintf(stderr, "invalid rate-control target\n");
            goto free_payload;
        }
        /* Fixed-level encoding gains nothing from retaining all quality rows. */
        options.workspace = rate_control_enabled ? &workspace : NULL;
        for (;;) {
            options.loss_level = rate_control_enabled
                                     ? rate_control.loss_level
                                     : current_loss_level;
            status = codec_supermovingblocks_encode_frame(
                &source, previous_arg, &options, &payload, &reconstructed,
                &stats);
            if (status != REPLAY_OK) {
                fprintf(stderr, "encoding failed: %s\n",
                        replay_status_string(status));
                goto free_payload;
            }
            if (self_check_frames) {
                status = codec_supermovingblocks_verify_frame(
                    payload.data, payload.size, previous_arg, &decoded,
                    &consumed_bits, NULL);
                if (status != REPLAY_OK ||
                    consumed_bits != stats.bits_written ||
                    memcmp(decoded_pixels, reconstructed_pixels,
                           pixel_count * sizeof(*decoded_pixels)) != 0) {
                    fprintf(stderr, "internal decode cross-check failed: %s\n",
                            replay_status_string(status));
                    goto free_payload;
                }
            }
            if (trace_frame(
                    trace, frame_number, width, height, &stats, &source,
                    &reconstructed, payload.size,
                    options.loss_level, retry,
                    target_bytes != 0U ? rate_control.target_min_bytes : 0U,
                    target_bytes != 0U ? rate_control.target_max_bytes : 0U,
                    options.policy, rate_search) !=
                EXIT_SUCCESS) {
                goto free_payload;
            }
            if (!rate_control_enabled) {
                break;
            }
            if (rate_search == RATE_SEARCH_BRACKETED) {
                unsigned next_level;

                if (!bracketed_rate_next(
                        &bracketed, &rate_control, payload.size,
                        options.loss_level, &next_level)) {
                    break;
                }
                rate_control.loss_level = next_level;
            } else if (mb_rate_control_observe(
                           &rate_control, payload.size) == MB_RATE_ACCEPT) {
                break;
            }
            ++retry;
        }
        current_loss_level = options.loss_level;
        if (payload_prefix != NULL) {
            if (make_frame_path(generated_payload, sizeof(generated_payload),
                                payload_prefix, frame_number, ".mb19") !=
                EXIT_SUCCESS) {
                goto free_payload;
            }
            frame_payload = generated_payload;
        }
        if (recon_prefix != NULL) {
            if (make_frame_path(generated_ppm, sizeof(generated_ppm),
                                recon_prefix, frame_number, ".ppm") !=
                EXIT_SUCCESS) {
                goto free_payload;
            }
            frame_ppm = generated_ppm;
        }
        if ((payload_path != NULL || payload_prefix != NULL) &&
            write_payload(frame_payload, &payload) != EXIT_SUCCESS) {
            goto free_payload;
        }
        if (frame_ppm != NULL &&
            write_reconstructed_ppm(frame_ppm, &reconstructed, rgb,
                                    (size_t)width * 3U) != EXIT_SUCCESS) {
            goto free_payload;
        }
        if (sinkp != NULL &&
            frame_sink_add(sinkp, &payload, &reconstructed) != EXIT_SUCCESS) {
            goto free_payload;
        }
        if (keys_prefix != NULL) {
            if (make_frame_path(generated_key, sizeof(generated_key),
                                keys_prefix, frame_number, ".key") !=
                    EXIT_SUCCESS ||
                write_key_frame(generated_key, &reconstructed) !=
                    EXIT_SUCCESS) {
                goto free_payload;
            }
        }
        printf("frame=%zu codec=19 name=\"Super Moving Blocks\" "
               "retry=%u loss_level=%u bits=%zu bytes=%zu data4x4=%zu "
               "stationary4x4=%zu temporal4x4=%zu spatial4x4=%zu "
               "split4x4=%zu data2x2=%zu stationary2x2=%zu "
               "temporal2x2=%zu spatial2x2=%zu verify=ok "
               "payload=\"%s\"\n",
               frame_number, retry, options.loss_level, stats.bits_written,
               payload.size,
               stats.data4x4_blocks, stats.stationary4x4_blocks,
               stats.temporal4x4_blocks, stats.spatial4x4_blocks,
               stats.split4x4_blocks, stats.data2x2_blocks,
               stats.stationary2x2_blocks, stats.temporal2x2_blocks,
               stats.spatial2x2_blocks, frame_payload);
        memcpy(previous_pixels, reconstructed_pixels,
               pixel_count * sizeof(*previous_pixels));
        ++frame_number;
        if (payload_path != NULL ||
            (frame_limit != 0U && frame_number == frame_limit)) {
            break;
        }
    }
    if (frame_number == 0U) {
        fprintf(stderr, "%s: no complete input frames\n", input_path);
        goto free_payload;
    }
    if ((payload_path != NULL || frame_limit != 0U) &&
        require_eof(input, input_path) != EXIT_SUCCESS) {
        goto free_payload;
    }
    if (write_pad_frames(19U, width, height, payload_prefix, ".mb19",
                         keys_prefix, encode_pad, &frame_number,
                         sinkp) != EXIT_SUCCESS) {
        goto free_payload;
    }
    replay_buffer_free(&payload);
    result = EXIT_SUCCESS;
    goto done;

free_payload:
    replay_buffer_free(&payload);
done:
    if (have_workspace) {
        codec_supermovingblocks_workspace_destroy(&workspace);
    }
    if (trace != NULL && fclose(trace) != 0 && result == EXIT_SUCCESS) {
        perror(trace_path);
        result = EXIT_FAILURE;
    }
    if (input != NULL && input != stdin && fclose(input) != 0 &&
        result == EXIT_SUCCESS) {
        perror(input_path);
        result = EXIT_FAILURE;
    }
    free(decoded_pixels);
    free(previous_pixels);
    free(reconstructed_pixels);
    free(source_pixels);
    free(rgb);
assemble:
    if (result == EXIT_SUCCESS && container.output_path != NULL) {
        if (pad_chunks) {
            unsigned eff_fpc = container.frames_per_chunk;

            if (pad_sink_for_chunks(&sink, codec, width, height,
                                    container.frames_per_chunk,
                                    container.audio_input_path != NULL,
                                    &eff_fpc) != EXIT_SUCCESS) {
                result = EXIT_FAILURE;
            } else {
                container.frames_per_chunk = eff_fpc;
            }
        }
        if (result == EXIT_SUCCESS) {
            result = assemble_container(&sink, &container);
        }
    }
    if (sinkp != NULL) {
        frame_sink_free(&sink);
    }
    return result;
}
