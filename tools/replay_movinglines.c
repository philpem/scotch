/*
 * replay-movinglines: command-line encode/decode for compression type 1,
 * Moving Lines (see docs/spec/type1-moving-lines.md). Moving Lines works on
 * opaque 15-bit pixels; this tool packs/unpacks them as RGB555 (R high, then G,
 * then B in the low five bits), which is one of the two colour models the format
 * advertises. The packing is a tool convention for getting RGB in and out; the
 * codec itself never interprets the pixel.
 *
 *   replay-movinglines encode --input FILE|- --size WxH [--frames N]
 *       [--payload-prefix PREFIX] [--recon-prefix PREFIX]
 *   replay-movinglines decode --payload FILE --size WxH [--previous FILE]
 *       [--output-ppm FILE] [--output-pixels FILE]
 *
 * `encode` reads raw RGB24 frames, encodes each against the previous
 * reconstruction, self-checks the round-trip, and writes per-frame `.mln`
 * payloads and/or reconstructed PPMs. `decode` decodes one frame (optionally
 * given a previous frame as raw little-endian 16-bit pixels) to a PPM and/or a
 * raw 16-bit pixel file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_movinglines.h"
#include "replay/replay_buffer.h"

static uint16_t rgb24_to_rgb555(const uint8_t *rgb)
{
    unsigned r = rgb[0] >> 3, g = rgb[1] >> 3, b = rgb[2] >> 3;

    return (uint16_t)((r << 10) | (g << 5) | b);
}

static void rgb555_to_rgb24(uint16_t pixel, uint8_t *rgb)
{
    rgb[0] = (uint8_t)(((pixel >> 10) & 0x1FU) << 3);
    rgb[1] = (uint8_t)(((pixel >> 5) & 0x1FU) << 3);
    rgb[2] = (uint8_t)((pixel & 0x1FU) << 3);
}

static int read_exact(FILE *file, uint8_t *data, size_t size, int *eof)
{
    size_t got = fread(data, 1, size, file);

    *eof = 0;
    if (got == 0U && feof(file)) {
        *eof = 1;
        return 0;
    }
    if (got != size) {
        fprintf(stderr, "short read: wanted %zu bytes, got %zu\n", size, got);
        return -1;
    }
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        perror(path);
        return -1;
    }
    if (size != 0U && fwrite(data, 1, size, file) != size) {
        perror(path);
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int write_ppm(const char *path, const uint16_t *pixels, unsigned width,
                     unsigned height)
{
    FILE *file = fopen(path, "wb");
    size_t count = (size_t)width * height;
    size_t i;

    if (file == NULL) {
        perror(path);
        return -1;
    }
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (i = 0; i < count; ++i) {
        uint8_t rgb[3];

        rgb555_to_rgb24(pixels[i], rgb);
        if (fwrite(rgb, 1, 3, file) != 3) {
            fclose(file);
            return -1;
        }
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int pixels_to_bytes(const uint16_t *pixels, size_t count, uint8_t *out)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        out[2 * i] = (uint8_t)(pixels[i] & 0xFFU);
        out[2 * i + 1] = (uint8_t)((pixels[i] >> 8) & 0xFFU);
    }
    return 0;
}

static int run_encode(int argc, char **argv);
static int run_decode(int argc, char **argv);

static const char *option(int argc, char **argv, const char *name)
{
    int i;

    for (i = 0; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int parse_size(const char *text, unsigned *width, unsigned *height)
{
    return text != NULL && sscanf(text, "%ux%u", width, height) == 2 &&
                   *width != 0U && *height != 0U
               ? 0
               : -1;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "encode") == 0) {
        return run_encode(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "decode") == 0) {
        return run_decode(argc, argv);
    }
    fprintf(stderr,
            "usage: replay-movinglines encode --input FILE|- --size WxH "
            "[--frames N] [--payload-prefix PREFIX] [--recon-prefix PREFIX]\n"
            "       replay-movinglines decode --payload FILE --size WxH "
            "[--previous FILE] [--output-ppm FILE] [--output-pixels FILE]\n");
    return EXIT_FAILURE;
}

static int run_encode(int argc, char **argv)
{
    const char *input_path = option(argc, argv, "--input");
    const char *payload_prefix = option(argc, argv, "--payload-prefix");
    const char *recon_prefix = option(argc, argv, "--recon-prefix");
    const char *frames_text = option(argc, argv, "--frames");
    unsigned width, height;
    size_t pixel_count, frame_limit, frame = 0;
    uint8_t *rgb = NULL;
    uint16_t *source = NULL, *previous = NULL, *decoded = NULL;
    ReplayBuffer payload;
    FILE *input = NULL;
    int result = EXIT_FAILURE;

    if (input_path == NULL || parse_size(option(argc, argv, "--size"),
                                         &width, &height) != 0) {
        fprintf(stderr, "encode: --input and --size WxH are required\n");
        return EXIT_FAILURE;
    }
    frame_limit = frames_text != NULL ? strtoul(frames_text, NULL, 10) : 0U;
    pixel_count = (size_t)width * height;
    rgb = malloc(pixel_count * 3U);
    source = malloc(pixel_count * sizeof(*source));
    previous = malloc(pixel_count * sizeof(*previous));
    decoded = malloc(pixel_count * sizeof(*decoded));
    replay_buffer_init(&payload);
    if (rgb == NULL || source == NULL || previous == NULL || decoded == NULL) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    input = strcmp(input_path, "-") == 0 ? stdin : fopen(input_path, "rb");
    if (input == NULL) {
        perror(input_path);
        goto done;
    }

    for (;;) {
        const uint16_t *prev_arg = frame == 0U ? NULL : previous;
        size_t i;
        int eof;

        if (read_exact(input, rgb, pixel_count * 3U, &eof) != 0) {
            goto done;
        }
        if (eof) {
            break;
        }
        for (i = 0; i < pixel_count; ++i) {
            source[i] = rgb24_to_rgb555(&rgb[3 * i]);
        }
        if (codec_movinglines_encode_frame(source, prev_arg, width, height,
                                           &payload) != REPLAY_OK) {
            fprintf(stderr, "frame %zu: encode failed\n", frame);
            goto done;
        }
        /* Round-trip self-check: the decode must reproduce the source. */
        if (codec_movinglines_decode_frame(payload.data, payload.size, prev_arg,
                                           decoded, width, height,
                                           NULL) != REPLAY_OK ||
            memcmp(decoded, source, pixel_count * sizeof(*source)) != 0) {
            fprintf(stderr, "frame %zu: self-check failed\n", frame);
            goto done;
        }
        if (payload_prefix != NULL) {
            char path[1024];

            snprintf(path, sizeof(path), "%s%06zu.mln", payload_prefix, frame);
            if (write_file(path, payload.data, payload.size) != 0) {
                goto done;
            }
        }
        if (recon_prefix != NULL) {
            char path[1024];

            snprintf(path, sizeof(path), "%s%06zu.ppm", recon_prefix, frame);
            if (write_ppm(path, decoded, width, height) != 0) {
                goto done;
            }
        }
        printf("frame=%zu codec=1 name=\"Moving Lines\" bytes=%zu verify=ok\n",
               frame, payload.size);
        memcpy(previous, decoded, pixel_count * sizeof(*previous));
        ++frame;
        if (frame_limit != 0U && frame >= frame_limit) {
            break;
        }
    }
    result = EXIT_SUCCESS;

done:
    if (input != NULL && input != stdin) {
        fclose(input);
    }
    replay_buffer_free(&payload);
    free(decoded);
    free(previous);
    free(source);
    free(rgb);
    return result;
}

static int run_decode(int argc, char **argv)
{
    const char *payload_path = option(argc, argv, "--payload");
    const char *previous_path = option(argc, argv, "--previous");
    const char *ppm_path = option(argc, argv, "--output-ppm");
    const char *pixels_path = option(argc, argv, "--output-pixels");
    unsigned width, height;
    size_t pixel_count, consumed = 0;
    uint16_t *previous = NULL, *decoded = NULL;
    uint8_t *raw = NULL, *out_bytes = NULL;
    ReplayBuffer payload;
    int result = EXIT_FAILURE;

    if (payload_path == NULL || parse_size(option(argc, argv, "--size"),
                                           &width, &height) != 0) {
        fprintf(stderr, "decode: --payload and --size WxH are required\n");
        return EXIT_FAILURE;
    }
    pixel_count = (size_t)width * height;
    decoded = malloc(pixel_count * sizeof(*decoded));
    replay_buffer_init(&payload);
    if (decoded == NULL) {
        fprintf(stderr, "out of memory\n");
        goto done;
    }
    {
        FILE *file = fopen(payload_path, "rb");
        long size;

        if (file == NULL) {
            perror(payload_path);
            goto done;
        }
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        raw = size > 0 ? malloc((size_t)size) : NULL;
        if (size > 0 && (raw == NULL ||
                         fread(raw, 1, (size_t)size, file) != (size_t)size)) {
            fprintf(stderr, "%s: read failed\n", payload_path);
            fclose(file);
            goto done;
        }
        fclose(file);
        (void)replay_buffer_append(&payload, raw, size > 0 ? (size_t)size : 0U);
    }
    if (previous_path != NULL) {
        FILE *file = fopen(previous_path, "rb");
        size_t i;
        uint8_t two[2];

        previous = malloc(pixel_count * sizeof(*previous));
        if (file == NULL || previous == NULL) {
            if (file != NULL) {
                fclose(file);
            }
            fprintf(stderr, "%s: cannot read previous frame\n", previous_path);
            goto done;
        }
        for (i = 0; i < pixel_count; ++i) {
            if (fread(two, 1, 2, file) != 2) {
                fprintf(stderr, "%s: too short\n", previous_path);
                fclose(file);
                goto done;
            }
            previous[i] = (uint16_t)(two[0] | (two[1] << 8));
        }
        fclose(file);
    }

    if (codec_movinglines_decode_frame(payload.data, payload.size, previous,
                                       decoded, width, height,
                                       &consumed) != REPLAY_OK) {
        fprintf(stderr, "decode failed\n");
        goto done;
    }
    if (ppm_path != NULL && write_ppm(ppm_path, decoded, width, height) != 0) {
        goto done;
    }
    if (pixels_path != NULL) {
        out_bytes = malloc(pixel_count * 2U);
        if (out_bytes == NULL) {
            goto done;
        }
        pixels_to_bytes(decoded, pixel_count, out_bytes);
        if (write_file(pixels_path, out_bytes, pixel_count * 2U) != 0) {
            goto done;
        }
    }
    printf("codec=1 name=\"Moving Lines\" size=%ux%u consumed=%zu status=ok\n",
           width, height, consumed);
    result = EXIT_SUCCESS;

done:
    replay_buffer_free(&payload);
    free(out_bytes);
    free(raw);
    free(decoded);
    free(previous);
    return result;
}
