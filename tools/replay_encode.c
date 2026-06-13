#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_movingblockshq.h"
#include "replay/codec_supermovingblocks.h"
#include "replay/mb_color.h"
#include "replay/mb_metrics.h"
#include "replay/mb_quality.h"
#include "replay/mb_rate_control.h"

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
            "usage: replay-encode --codec 17|19 --input FILE|- --size WxH "
            "(--payload FILE | --payload-prefix PREFIX) "
            "[--input-format rgb24|6y5uv|yuv555] [--dither 4x4|8x8|--no-dither] "
            "[--frames N] [--data-only] [--loss-level 0..28] "
            "[--policy ordered|lowest-error] "
            "[--rate-search linear|bracketed] "
            "[--target-bytes N] [--trace FILE] "
            "[--recon-ppm FILE | --recon-prefix PREFIX] "
            "[--keys-prefix PREFIX]\n");
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
 * Write a reconstructed frame as packed 6Y5UV halfwords (Y[0:5], U[6:10],
 * V[11:15], little-endian) -- the native key-frame format the Replay player
 * expands when starting decompression at a chunk boundary.
 */
static int write_key_frame(const char *path, const MbFrame *frame)
{
    FILE *file = fopen(path, "wb");
    unsigned y;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;

        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *p = &frame->pixels[(size_t)y * frame->stride + x];
            unsigned packed = ((unsigned)p->y & 0x3FU) |
                              (((unsigned)p->u & 0x1FU) << 6U) |
                              (((unsigned)p->v & 0x1FU) << 11U);
            uint8_t bytes[2] = {
                (uint8_t)(packed & 0xFFU), (uint8_t)((packed >> 8U) & 0xFFU)
            };

            if (fwrite(bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
        }
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/*
 * Write a reconstructed frame as packed YUV555 halfwords (Y[0:4], U[5:9],
 * V[10:14], little-endian) -- the native type 17 key-frame format the player
 * loads when starting decompression at a chunk boundary.
 */
static int write_key_frame_yuv555(const char *path, const MbFrame *frame)
{
    FILE *file = fopen(path, "wb");
    unsigned y;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;

        for (x = 0U; x < frame->width; ++x) {
            const MbPixel *p = &frame->pixels[(size_t)y * frame->stride + x];
            unsigned packed = ((unsigned)p->y & 0x1FU) |
                              (((unsigned)p->u & 0x1FU) << 5U) |
                              (((unsigned)p->v & 0x1FU) << 10U);
            uint8_t bytes[2] = {
                (uint8_t)(packed & 0xFFU), (uint8_t)((packed >> 8U) & 0xFFU)
            };

            if (fwrite(bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
        }
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
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

/*
 * Self-contained type 17 (Moving Blocks HQ) encode loop. RGB input is converted
 * to YUV555; copy modes are enabled for inter frames (stationary/temporal need
 * the previous reconstruction) with spatial and split also legal in the key
 * frame. Each frame is independently decoded as a self-check.
 */
static int run_encode17(const char *input_path, InputFormat input_format,
                        unsigned width, unsigned height,
                        const char *payload_path, const char *payload_prefix,
                        size_t frame_limit, unsigned loss_level, int data_only,
                        const char *recon_prefix, const char *recon_ppm_path,
                        const char *keys_prefix, size_t target_bytes,
                        MbColorDither dither)
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
        CodecMovingBlocksHqEncodeOptions options;
        CodecMovingBlocksHqEncodeStats stats;
        unsigned retry = 0U;
        ReplayStatus status;

        if (read_result == FRAME_READ_EOF) {
            break;
        }
        if (read_result != FRAME_READ_OK) {
            goto cleanup;
        }
        status = input_format == INPUT_RGB24
                     ? mb_color_rgb24_to_yuv555(rgb, (size_t)width * 3U,
                                                &source, dither)
                     : unpack_yuv555(rgb, &source);
        if (status != REPLAY_OK) {
            fprintf(stderr, "frame %zu: input conversion failed: %s\n",
                    frame_number, replay_status_string(status));
            goto cleanup;
        }

        options.allow_stationary = !data_only && previous_arg != NULL;
        options.allow_temporal = !data_only && previous_arg != NULL;
        options.allow_spatial = !data_only;
        options.allow_split = !data_only;

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
                status = codec_movingblockshq_encode_frame(
                    &source, previous_arg, &options, &payload, &reconstructed,
                    &stats);
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
        status = codec_movingblockshq_verify_frame(
            payload.data, payload.size, previous_arg, &decoded, NULL, NULL);
        if (status != REPLAY_OK ||
            memcmp(decoded_pixels, recon_pixels,
                   pixel_count * sizeof(*recon_pixels)) != 0) {
            fprintf(stderr, "frame %zu: decode self-check failed\n",
                    frame_number);
            goto cleanup;
        }

        printf("frame=%zu codec=17 name=\"Moving Blocks HQ\" retry=%u "
               "loss_level=%u bits=%zu bytes=%zu data4x4=%zu stationary4x4=%zu "
               "temporal4x4=%zu spatial4x4=%zu split4x4=%zu data2x2=%zu "
               "stationary2x2=%zu temporal2x2=%zu spatial2x2=%zu verify=ok\n",
               frame_number, retry, options.loss_level, stats.bits_written,
               payload.size, stats.data4x4_blocks, stats.stationary4x4_blocks,
               stats.temporal4x4_blocks, stats.spatial4x4_blocks,
               stats.split4x4_blocks, stats.data2x2_blocks,
               stats.stationary2x2_blocks, stats.temporal2x2_blocks,
               stats.spatial2x2_blocks);

        {
            char generated[1024];
            const char *frame_payload = payload_path;

            if (payload_prefix != NULL) {
                if (make_frame_path(generated, sizeof(generated),
                                    payload_prefix, frame_number, ".mb17") !=
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
            if (write_yuv555_ppm(frame_ppm, &reconstructed, rgb,
                                 (size_t)width * 3U) != EXIT_SUCCESS) {
                goto cleanup;
            }
        }
        if (keys_prefix != NULL) {
            char generated[1024];

            if (make_frame_path(generated, sizeof(generated), keys_prefix,
                                frame_number, ".key") != EXIT_SUCCESS ||
                write_key_frame_yuv555(generated, &reconstructed) !=
                    EXIT_SUCCESS) {
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
    FILE *input = NULL;
    FILE *trace = NULL;
    size_t pixel_count;
    size_t rgb_size;
    size_t frame_number = 0U;
    int result = EXIT_FAILURE;
    int i;

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
        } else if (strcmp(argv[i], "--no-dither") == 0) {
            dither = MB_COLOR_DITHER_NONE;
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
    if (codec == 17U) {
        if (input_path == NULL ||
            (payload_path == NULL) == (payload_prefix == NULL) ||
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
        return run_encode17(input_path, input_format, width, height,
                            payload_path, payload_prefix, frame_limit,
                            loss_level, data_only, recon_prefix,
                            recon_ppm_path, keys_prefix, target_bytes, dither);
    }
    if (codec != 19U || input_path == NULL ||
        (payload_path == NULL) == (payload_prefix == NULL) ||
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
    if (codec_supermovingblocks_workspace_init(
            &workspace, width, height) != REPLAY_OK) {
        fprintf(stderr, "unable to allocate encoder search workspace\n");
        goto free_payload;
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
        codec_supermovingblocks_workspace_reset(&workspace);
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
            status = codec_supermovingblocks_verify_frame(
                payload.data, payload.size, previous_arg, &decoded,
                &consumed_bits, NULL);
            if (status != REPLAY_OK || consumed_bits != stats.bits_written ||
                memcmp(decoded_pixels, reconstructed_pixels,
                       pixel_count * sizeof(*decoded_pixels)) != 0) {
                fprintf(stderr, "internal decode cross-check failed: %s\n",
                        replay_status_string(status));
                goto free_payload;
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
        if (write_payload(frame_payload, &payload) != EXIT_SUCCESS ||
            write_reconstructed_ppm(frame_ppm, &reconstructed, rgb,
                                    (size_t)width * 3U) != EXIT_SUCCESS) {
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
    replay_buffer_free(&payload);
    result = EXIT_SUCCESS;
    goto done;

free_payload:
    replay_buffer_free(&payload);
done:
    codec_supermovingblocks_workspace_destroy(&workspace);
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
    return result;
}
