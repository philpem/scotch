#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_color.h"
#include "replay/mb_rate_control.h"

/*
 * This program is intentionally a thin development harness around the codec:
 *
 *   RGB24 frame -> CompLib-style 6Y5UV -> encode -> independent decode
 *               -> compare reconstruction -> write raw payload
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

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-encode --codec 19 --input FILE|- --size WxH "
            "(--payload FILE | --payload-prefix PREFIX) "
            "[--frames N] [--data-only] [--loss-level 0..28] "
            "[--target-bytes N] [--trace FILE] "
            "[--recon-ppm FILE | --recon-prefix PREFIX]\n");
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
            fprintf(stderr, "%s: truncated RGB24 frame (%zu of %zu bytes)\n",
                    name, offset, size);
            return FRAME_READ_ERROR;
        }
        offset += count;
    }
    return FRAME_READ_OK;
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

static int trace_frame(FILE *trace, size_t frame_number, unsigned width,
                       unsigned height,
                       const CodecSuperMovingBlocksEncodeStats *stats,
                       size_t bytes, unsigned loss_level, unsigned retry,
                       size_t target_min, size_t target_max)
{
    if (trace == NULL) {
        return EXIT_SUCCESS;
    }
    if (fprintf(trace,
                "frame=%zu codec=19 size=%ux%u retry=%u loss_level=%u "
                "target_min=%zu target_max=%zu data4x4=%zu "
                "stationary4x4=%zu temporal4x4=%zu spatial4x4=%zu "
                "split4x4=%zu data2x2=%zu stationary2x2=%zu "
                "temporal2x2=%zu spatial2x2=%zu "
                "bits=%zu bytes=%zu "
                "verify=ok\n",
                frame_number, width, height, retry, loss_level,
                target_min, target_max, stats->data4x4_blocks,
                stats->stationary4x4_blocks, stats->temporal4x4_blocks,
                stats->spatial4x4_blocks, stats->split4x4_blocks,
                stats->data2x2_blocks, stats->stationary2x2_blocks,
                stats->temporal2x2_blocks, stats->spatial2x2_blocks,
                stats->bits_written, bytes) < 0) {
        perror("trace output");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *payload_path = NULL;
    const char *payload_prefix = NULL;
    const char *trace_path = NULL;
    const char *recon_ppm_path = NULL;
    const char *recon_prefix = NULL;
    unsigned width = 0U;
    unsigned height = 0U;
    unsigned codec = 0U;
    unsigned loss_level = 0U;
    unsigned current_loss_level;
    size_t target_bytes = 0U;
    size_t frame_limit = 0U;
    int data_only = 0;
    uint8_t *rgb = NULL;
    MbPixel *source_pixels = NULL;
    MbPixel *reconstructed_pixels = NULL;
    MbPixel *previous_pixels = NULL;
    MbPixel *decoded_pixels = NULL;
    ReplayBuffer payload;
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
    if (codec != 19U || input_path == NULL ||
        (payload_path == NULL) == (payload_prefix == NULL) ||
        (recon_ppm_path != NULL && recon_prefix != NULL) ||
        (payload_path != NULL && (frame_limit > 1U || recon_prefix != NULL)) ||
        (payload_prefix != NULL && recon_ppm_path != NULL) ||
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
            current_loss_level
        };
        CodecSuperMovingBlocksEncodeStats stats;
        const MbFrame *previous_arg = frame_number == 0U ? NULL : &previous;
        char generated_payload[4096];
        char generated_ppm[4096];
        const char *frame_payload = payload_path;
        const char *frame_ppm = recon_ppm_path;
        size_t consumed_bits;
        MbRateControl rate_control;
        int rate_control_enabled = target_bytes != 0U && frame_number != 0U;
        unsigned retry = 0U;
        ReplayStatus status;

        if (read_result == FRAME_READ_EOF) {
            break;
        }
        if (read_result == FRAME_READ_ERROR) {
            goto free_payload;
        }
        status = mb_color_rgb24_to_6y5uv(
            rgb, (size_t)width * 3U, &source);
        if (status != REPLAY_OK) {
            fprintf(stderr, "RGB conversion failed: %s\n",
                    replay_status_string(status));
            goto free_payload;
        }
        if (target_bytes != 0U &&
            mb_rate_control_init(&rate_control, target_bytes,
                                 current_loss_level) != REPLAY_OK) {
            fprintf(stderr, "invalid rate-control target\n");
            goto free_payload;
        }
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
                    trace, frame_number, width, height, &stats, payload.size,
                    options.loss_level, retry,
                    target_bytes != 0U ? rate_control.target_min_bytes : 0U,
                    target_bytes != 0U ? rate_control.target_max_bytes : 0U) !=
                EXIT_SUCCESS) {
                goto free_payload;
            }
            if (!rate_control_enabled ||
                mb_rate_control_observe(&rate_control, payload.size) ==
                    MB_RATE_ACCEPT) {
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
        printf("frame=%zu codec=19 retry=%u loss_level=%u bits=%zu bytes=%zu data4x4=%zu "
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
        fprintf(stderr, "%s: no complete RGB24 frames\n", input_path);
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
