#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_supermovingblocks.h"
#include "replay/mb_color.h"

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: replay-encode --codec 19 --input FILE|- --size WIDTHxHEIGHT "
            "--payload FILE [--trace FILE] [--recon-ppm FILE]\n");
}

static int read_exact_frame(const char *path, uint8_t *data, size_t size)
{
    FILE *file = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    size_t offset = 0U;
    int result = EXIT_FAILURE;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    while (offset < size) {
        size_t count = fread(data + offset, 1U, size - offset, file);
        if (count == 0U) {
            if (ferror(file)) {
                perror(path);
            } else {
                fprintf(stderr, "%s: truncated RGB24 frame (%zu of %zu bytes)\n",
                        path, offset, size);
            }
            goto done;
        }
        offset += count;
    }
    {
        uint8_t extra;
        size_t count = fread(&extra, 1U, 1U, file);
        if (count != 0U) {
            fprintf(stderr, "%s: input contains more than one RGB24 frame\n",
                    path);
            goto done;
        }
        if (ferror(file)) {
            perror(path);
            goto done;
        }
    }
    result = EXIT_SUCCESS;

done:
    if (file != stdin && fclose(file) != 0 && result == EXIT_SUCCESS) {
        perror(path);
        result = EXIT_FAILURE;
    }
    return result;
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

static int write_trace(const char *path, unsigned width, unsigned height,
                       size_t bits, size_t bytes)
{
    FILE *file;

    if (path == NULL) {
        return EXIT_SUCCESS;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }
    fprintf(file,
            "frame=0 codec=19 size=%ux%u mode=data4x4 blocks=%u "
            "bits=%zu bytes=%zu verify=ok\n",
            width, height, (width / 4U) * (height / 4U), bits, bytes);
    if (fclose(file) != 0) {
        perror(path);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
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

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *payload_path = NULL;
    const char *trace_path = NULL;
    const char *recon_ppm_path = NULL;
    unsigned width = 0U;
    unsigned height = 0U;
    unsigned codec = 0U;
    uint8_t *rgb = NULL;
    MbPixel *source_pixels = NULL;
    MbPixel *reconstructed_pixels = NULL;
    MbPixel *decoded_pixels = NULL;
    ReplayBuffer payload;
    MbFrame source;
    MbFrame reconstructed;
    MbFrame decoded;
    size_t pixel_count;
    size_t rgb_size;
    size_t bits_written;
    size_t bits_consumed;
    ReplayStatus status;
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
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--recon-ppm") == 0 && i + 1 < argc) {
            recon_ppm_path = argv[++i];
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
    if (codec != 19U || input_path == NULL || payload_path == NULL ||
        width == 0U || height == 0U || (width & 3U) != 0U ||
        (height & 3U) != 0U || (size_t)width > SIZE_MAX / (size_t)height) {
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
    decoded_pixels = malloc(pixel_count * sizeof(*decoded_pixels));
    if (rgb == NULL || source_pixels == NULL || reconstructed_pixels == NULL ||
        decoded_pixels == NULL) {
        fprintf(stderr, "unable to allocate frame buffers\n");
        goto done;
    }
    if (read_exact_frame(input_path, rgb, rgb_size) != EXIT_SUCCESS) {
        goto done;
    }

    source = (MbFrame){ width, height, width, source_pixels };
    reconstructed =
        (MbFrame){ width, height, width, reconstructed_pixels };
    decoded = (MbFrame){ width, height, width, decoded_pixels };
    status = mb_color_rgb24_to_6y5uv(rgb, (size_t)width * 3U, &source);
    if (status != REPLAY_OK) {
        fprintf(stderr, "RGB conversion failed: %s\n",
                replay_status_string(status));
        goto done;
    }
    replay_buffer_init(&payload);
    status = codec_supermovingblocks_encode_data_frame(
        &source, &payload, &reconstructed, &bits_written);
    if (status != REPLAY_OK) {
        fprintf(stderr, "encoding failed: %s\n", replay_status_string(status));
        replay_buffer_free(&payload);
        goto done;
    }
    status = codec_supermovingblocks_verify_frame(
        payload.data, payload.size, NULL, &decoded, &bits_consumed, NULL);
    if (status != REPLAY_OK || bits_consumed != bits_written ||
        memcmp(decoded_pixels, reconstructed_pixels,
               pixel_count * sizeof(*decoded_pixels)) != 0) {
        fprintf(stderr, "internal decode cross-check failed: %s\n",
                replay_status_string(status));
        replay_buffer_free(&payload);
        goto done;
    }
    if (write_payload(payload_path, &payload) != EXIT_SUCCESS ||
        write_trace(trace_path, width, height, bits_written,
                    payload.size) != EXIT_SUCCESS ||
        write_reconstructed_ppm(recon_ppm_path, &reconstructed, rgb,
                                (size_t)width * 3U) != EXIT_SUCCESS) {
        replay_buffer_free(&payload);
        goto done;
    }
    printf("codec=19 size=%ux%u bits=%zu bytes=%zu verify=ok payload=\"%s\"\n",
           width, height, bits_written, payload.size, payload_path);
    replay_buffer_free(&payload);
    result = EXIT_SUCCESS;

done:
    free(decoded_pixels);
    free(reconstructed_pixels);
    free(source_pixels);
    free(rgb);
    return result;
}
