#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_codec.h"
#include "replay/mb_metrics.h"
#include "replay/replay_buffer.h"

/*
 * Verification is kept as a separate executable and decode path so encoder
 * bugs cannot be hidden by inspecting only encoder-owned reconstruction. Raw
 * temporal payloads need a caller-provided previous frame, which this simple
 * one-frame CLI deliberately does not synthesize.
 */
static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-verify --codec ID [--describe|--verify-huffman] "
            "[--payload FILE --size WIDTHxHEIGHT [--previous-6y5uv FILE] "
            "[--output-6y5uv FILE] [--expect-6y5uv FILE] "
            "[--reference-6y5uv FILE] [--trace FILE]]\n");
}

static const char *mode_name(CodecSuperMovingBlocksMode mode)
{
    switch (mode) {
    case CODEC_SUPERMOVINGBLOCKS_MODE_DATA:
        return "data";
    case CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY:
        return "stationary";
    case CODEC_SUPERMOVINGBLOCKS_MODE_TEMPORAL:
        return "temporal";
    case CODEC_SUPERMOVINGBLOCKS_MODE_SPATIAL:
        return "spatial";
    case CODEC_SUPERMOVINGBLOCKS_MODE_SPLIT:
        return "split";
    default:
        return "unknown";
    }
}

typedef struct {
    FILE *file;
    const MbFrame *reference;
    const MbFrame *decoded;
} DecodeTraceContext;

static void trace_decoded_block(
    const CodecSuperMovingBlocksDecodeEvent *event, void *opaque)
{
    DecodeTraceContext *context = opaque;

    fprintf(context->file,
            "codec=19 name=\"Super Moving Blocks\" x=%u y=%u size=%u "
            "mode=%s bit_start=%zu bit_end=%zu bits=%zu dx=%d dy=%d",
            event->x, event->y, event->size, mode_name(event->mode),
            event->bit_start, event->bit_end,
            event->bit_end - event->bit_start,
            event->motion_dx, event->motion_dy);
    if (context->reference != NULL) {
        MbFrameMetrics metrics;

        if (mb_metrics_compare_6y5uv_region(
                context->reference, context->decoded,
                event->x, event->y, event->size, event->size,
                &metrics) == REPLAY_OK) {
            fprintf(context->file,
                    " sse_y=%llu sse_u=%llu sse_v=%llu "
                    "mse_y=%.6f mse_u=%.6f mse_v=%.6f "
                    "max_error_y=%u max_error_u=%u max_error_v=%u",
                    (unsigned long long)metrics.squared_error_y,
                    (unsigned long long)metrics.squared_error_u,
                    (unsigned long long)metrics.squared_error_v,
                    mb_metrics_mse(metrics.squared_error_y,
                                   metrics.pixel_count),
                    mb_metrics_mse(metrics.squared_error_u,
                                   metrics.pixel_count),
                    mb_metrics_mse(metrics.squared_error_v,
                                   metrics.pixel_count),
                    metrics.max_error_y, metrics.max_error_u,
                    metrics.max_error_v);
        }
    }
    fputc('\n', context->file);
}

static int read_file(const char *path, ReplayBuffer *buffer)
{
    uint8_t chunk[4096];
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (;;) {
        size_t count = fread(chunk, 1U, sizeof(chunk), file);
        if (count != 0U &&
            replay_buffer_append(buffer, chunk, count) != REPLAY_OK) {
            fprintf(stderr, "unable to allocate payload buffer\n");
            fclose(file);
            return EXIT_FAILURE;
        }
        if (count < sizeof(chunk)) {
            if (ferror(file)) {
                perror(path);
                fclose(file);
                return EXIT_FAILURE;
            }
            break;
        }
    }
    fclose(file);
    return EXIT_SUCCESS;
}

static int read_6y5uv(const char *path, MbFrame *frame)
{
    FILE *file = fopen(path, "rb");
    unsigned y;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    for (y = 0U; y < frame->height; ++y) {
        unsigned x;
        for (x = 0U; x < frame->width; ++x) {
            MbPixel *pixel = &frame->pixels[(size_t)y * frame->stride + x];
            uint8_t packed[3];

            if (fread(packed, 1U, sizeof(packed), file) != sizeof(packed)) {
                fprintf(stderr, "%s: truncated 6Y5UV frame at %u,%u\n",
                        path, x, y);
                fclose(file);
                return EXIT_FAILURE;
            }
            pixel->y = packed[0];
            pixel->u = packed[1];
            pixel->v = packed[2];
        }
    }
    if (fgetc(file) != EOF || ferror(file)) {
        fprintf(stderr, "%s: trailing data in 6Y5UV frame\n", path);
        fclose(file);
        return EXIT_FAILURE;
    }
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int write_6y5uv(const char *path, const MbFrame *frame)
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
            const MbPixel *pixel =
                &frame->pixels[(size_t)y * frame->stride + x];
            uint8_t packed[3] = { pixel->y, pixel->u, pixel->v };

            if (fwrite(packed, 1U, sizeof(packed), file) != sizeof(packed)) {
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

static int compare_6y5uv(const MbFrame *actual, const MbFrame *expected,
                         const char *expected_path)
{
    unsigned y;

    for (y = 0U; y < actual->height; ++y) {
        unsigned x;
        for (x = 0U; x < actual->width; ++x) {
            const MbPixel *a =
                &actual->pixels[(size_t)y * actual->stride + x];
            const MbPixel *e =
                &expected->pixels[(size_t)y * expected->stride + x];

            if (a->y != e->y || a->u != e->u || a->v != e->v) {
                fprintf(stderr,
                        "%s: decoded mismatch at %u,%u: "
                        "actual=%u,%u,%u expected=%u,%u,%u\n",
                        expected_path, x, y, a->y, a->u, a->v,
                        e->y, e->u, e->v);
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}

static int verify_payload_file(const MbCodec *codec, const char *path,
                               unsigned width, unsigned height,
                               const char *previous_path,
                               const char *output_path,
                               const char *expected_path,
                               const char *reference_path,
                               const char *trace_path)
{
    ReplayBuffer payload;
    MbFrame frame;
    MbFrame previous = { 0U, 0U, 0U, NULL };
    MbFrame expected = { 0U, 0U, 0U, NULL };
    MbFrame reference = { 0U, 0U, 0U, NULL };
    MbVerifyError error;
    size_t bits;
    ReplayStatus status;
    size_t pixel_count;
    FILE *trace = NULL;
    DecodeTraceContext trace_context;

    if (codec->id != REPLAY_CODEC_SUPERMOVINGBLOCKS) {
        fprintf(stderr, "payload verification is not implemented for %s\n",
                codec->name);
        return EXIT_FAILURE;
    }
    if (width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        fprintf(stderr, "invalid frame dimensions\n");
        return EXIT_FAILURE;
    }
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(*frame.pixels)) {
        fprintf(stderr, "frame dimensions are too large\n");
        return EXIT_FAILURE;
    }

    replay_buffer_init(&payload);
    if (read_file(path, &payload) != EXIT_SUCCESS) {
        replay_buffer_free(&payload);
        return EXIT_FAILURE;
    }
    frame.width = width;
    frame.height = height;
    frame.stride = width;
    frame.pixels = calloc(pixel_count, sizeof(*frame.pixels));
    if (frame.pixels == NULL) {
        fprintf(stderr, "unable to allocate decoded frame\n");
        replay_buffer_free(&payload);
        return EXIT_FAILURE;
    }

    if (previous_path != NULL) {
        previous = (MbFrame){ width, height, width, NULL };
        previous.pixels = malloc(pixel_count * sizeof(*previous.pixels));
        if (previous.pixels == NULL ||
            read_6y5uv(previous_path, &previous) != EXIT_SUCCESS) {
            free(previous.pixels);
            free(frame.pixels);
            replay_buffer_free(&payload);
            return EXIT_FAILURE;
        }
    }

    if (reference_path != NULL) {
        reference = (MbFrame){ width, height, width, NULL };
        reference.pixels = malloc(pixel_count * sizeof(*reference.pixels));
        if (reference.pixels == NULL ||
            read_6y5uv(reference_path, &reference) != EXIT_SUCCESS) {
            free(reference.pixels);
            free(previous.pixels);
            free(frame.pixels);
            replay_buffer_free(&payload);
            return EXIT_FAILURE;
        }
    }

    if (trace_path != NULL) {
        trace = fopen(trace_path, "w");
        if (trace == NULL) {
            perror(trace_path);
            free(reference.pixels);
            free(previous.pixels);
            free(frame.pixels);
            replay_buffer_free(&payload);
            return EXIT_FAILURE;
        }
    }
    trace_context.file = trace;
    trace_context.reference = reference.pixels != NULL ? &reference : NULL;
    trace_context.decoded = &frame;

    status = codec_supermovingblocks_verify_frame_traced(
        payload.data, payload.size,
        previous.pixels != NULL ? &previous : NULL, &frame, &bits, &error,
        trace != NULL ? trace_decoded_block : NULL, &trace_context);
    if (status == REPLAY_OK) {
        printf("codec=%u name=\"%s\" payload=\"%s\" size=%ux%u "
               "bits=%zu status=ok\n",
               (unsigned)codec->id, codec->name, path, width, height, bits);
    } else {
        fprintf(stderr,
                "payload failed: %s bit=%zu block=%u,%u detail=%s\n",
                replay_status_string(status), error.bit_position,
                error.block_x, error.block_y,
                error.detail == NULL ? "unspecified" : error.detail);
    }
    if (status == REPLAY_OK && output_path != NULL &&
        write_6y5uv(output_path, &frame) != EXIT_SUCCESS) {
        status = REPLAY_INVALID_ARGUMENT;
    }
    if (status == REPLAY_OK && expected_path != NULL) {
        expected = (MbFrame){ width, height, width, NULL };
        expected.pixels = malloc(pixel_count * sizeof(*expected.pixels));
        if (expected.pixels == NULL ||
            read_6y5uv(expected_path, &expected) != EXIT_SUCCESS ||
            compare_6y5uv(&frame, &expected, expected_path) != EXIT_SUCCESS) {
            status = REPLAY_INVALID_ARGUMENT;
        }
    }
    if (status == REPLAY_OK && reference_path != NULL) {
        MbFrameMetrics metrics;

        if (mb_metrics_compare_6y5uv(&reference, &frame, &metrics) !=
            REPLAY_OK) {
            status = REPLAY_INVALID_ARGUMENT;
        } else {
            printf("reference=\"%s\" sse_y=%llu sse_u=%llu sse_v=%llu "
                   "mse_y=%.6f mse_u=%.6f mse_v=%.6f "
                   "psnr_y=%.6f psnr_u=%.6f psnr_v=%.6f "
                   "max_error_y=%u max_error_u=%u max_error_v=%u\n",
                   reference_path,
                   (unsigned long long)metrics.squared_error_y,
                   (unsigned long long)metrics.squared_error_u,
                   (unsigned long long)metrics.squared_error_v,
                   mb_metrics_mse(metrics.squared_error_y,
                                  metrics.pixel_count),
                   mb_metrics_mse(metrics.squared_error_u,
                                  metrics.pixel_count),
                   mb_metrics_mse(metrics.squared_error_v,
                                  metrics.pixel_count),
                   mb_metrics_psnr(metrics.squared_error_y,
                                   metrics.pixel_count, 63U),
                   mb_metrics_psnr(metrics.squared_error_u,
                                   metrics.pixel_count, 31U),
                   mb_metrics_psnr(metrics.squared_error_v,
                                   metrics.pixel_count, 31U),
                   metrics.max_error_y, metrics.max_error_u,
                   metrics.max_error_v);
        }
    }
    free(reference.pixels);
    free(expected.pixels);
    free(previous.pixels);
    free(frame.pixels);
    replay_buffer_free(&payload);
    if (trace != NULL && ferror(trace)) {
        fprintf(stderr, "%s: trace write failed\n", trace_path);
        status = REPLAY_INVALID_ARGUMENT;
    }
    if (trace != NULL && fclose(trace) != 0) {
        perror(trace_path);
        status = REPLAY_INVALID_ARGUMENT;
    }
    return status == REPLAY_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int verify_huffman(const MbCodec *codec)
{
    unsigned expected;

    if (codec->luma_huffman == NULL) {
        fprintf(stderr, "%s has no implemented luma Huffman table\n",
                codec->name);
        return EXIT_FAILURE;
    }
    if (mb_huffman_validate(codec->luma_huffman) != REPLAY_OK) {
        fprintf(stderr, "%s luma Huffman table is invalid\n", codec->name);
        return EXIT_FAILURE;
    }

    for (expected = 0; expected < codec->luma_huffman->symbol_count;
         ++expected) {
        ReplayBuffer buffer;
        ReplayBitWriter writer;
        ReplayBitReader reader;
        unsigned actual;
        ReplayStatus status;

        replay_buffer_init(&buffer);
        replay_bitwriter_init(&writer, &buffer);
        status = mb_huffman_write(&writer, codec->luma_huffman, expected);
        if (status == REPLAY_OK) {
            status = replay_bitwriter_flush_zero(&writer);
        }
        if (status == REPLAY_OK) {
            replay_bitreader_init(&reader, buffer.data, buffer.size);
            status = mb_huffman_read(&reader, codec->luma_huffman, &actual);
        }
        replay_buffer_free(&buffer);

        if (status != REPLAY_OK || actual != expected) {
            fprintf(stderr, "symbol %u failed: %s\n", expected,
                    replay_status_string(status));
            return EXIT_FAILURE;
        }
    }

    printf("codec=%u name=\"%s\" huffman_symbols=%zu status=ok\n",
           (unsigned)codec->id, codec->name,
           codec->luma_huffman->symbol_count);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    const MbCodec *codec = NULL;
    int describe = 0;
    int huffman = 0;
    const char *payload_path = NULL;
    const char *previous_path = NULL;
    const char *output_path = NULL;
    const char *expected_path = NULL;
    const char *reference_path = NULL;
    const char *trace_path = NULL;
    unsigned width = 0;
    unsigned height = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            char *end;
            unsigned long id = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || id > 999UL) {
                usage(stderr);
                return EXIT_FAILURE;
            }
            codec = mb_codec_find((unsigned)id);
        } else if (strcmp(argv[i], "--describe") == 0) {
            describe = 1;
        } else if (strcmp(argv[i], "--verify-huffman") == 0) {
            huffman = 1;
        } else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            payload_path = argv[++i];
        } else if (strcmp(argv[i], "--previous-6y5uv") == 0 && i + 1 < argc) {
            previous_path = argv[++i];
        } else if (strcmp(argv[i], "--output-6y5uv") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--expect-6y5uv") == 0 && i + 1 < argc) {
            expected_path = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--reference-6y5uv") == 0 &&
                   i + 1 < argc) {
            reference_path = argv[++i];
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

    if (codec == NULL || (!describe && !huffman && payload_path == NULL) ||
        ((previous_path != NULL || output_path != NULL ||
          expected_path != NULL || reference_path != NULL ||
          trace_path != NULL) &&
         payload_path == NULL) ||
        (payload_path != NULL && (width == 0U || height == 0U))) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (describe) {
        printf("codec=%u name=\"%s\" bits=%u,%u,%u block=%ux%u motion=%ux%u\n",
               (unsigned)codec->id, codec->name,
               codec->y_bits, codec->u_bits, codec->v_bits,
               codec->block_width, codec->block_height,
               codec->max_motion_x, codec->max_motion_y);
    }
    if (huffman && verify_huffman(codec) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return payload_path == NULL
               ? EXIT_SUCCESS
               : verify_payload_file(codec, payload_path, width, height,
                                     previous_path, output_path,
                                     expected_path, reference_path,
                                     trace_path);
}
