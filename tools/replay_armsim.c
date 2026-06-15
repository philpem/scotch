/* replay-armsim -- run a compiled Acorn Replay decompressor over a payload.
 *
 * A native C replacement for the Unicorn-based Python harnesses
 * (tools/decomp19_unicorn.py and tools/movinglines_unicorn.py). It runs the
 * compiled CodecIf module under the vendored ARMulator, which models the
 * classic Acorn ARM memory semantics (unaligned LDR rotate; unaligned LDM
 * ignoring the low address bits) directly -- so none of the per-codec
 * instruction-signature alignment shims the Python harnesses needed exist here.
 *
 * The decompressor is left unpatched, so output words carry the codec's native
 * packed components; --output-layout selects how they are written.
 */

#include "replay/codecif.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "replay-armsim";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;
    if (f == NULL)
        die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0)
        die("cannot size %s", path);
    rewind(f);
    buf = malloc((size_t)size ? (size_t)size : 1);
    if (buf == NULL)
        die("out of memory reading %s", path);
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)
        die("short read on %s", path);
    fclose(f);
    *len_out = (size_t)size;
    return buf;
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        die("cannot create %s", path);
    if (len > 0 && fwrite(data, 1, len, f) != len)
        die("short write on %s", path);
    fclose(f);
}

static uint8_t *read_exact(const char *path, size_t want, const char *desc)
{
    size_t got;
    uint8_t *data = read_file(path, &got);
    if (got != want)
        die("%s: %s is %zu bytes; expected %zu", path, desc, got, want);
    return data;
}

static void numbered_path(char *out, size_t outlen, const char *prefix,
                          unsigned frame, const char *suffix)
{
    snprintf(out, outlen, "%s%06u%s", prefix, frame, suffix);
}

static ReplayPixelLayout layout_from_name(const char *name, int is_output)
{
    if (strcmp(name, "6y5uv") == 0) return REPLAY_PIX_6Y5UV;
    if (strcmp(name, "yuv555") == 0) return REPLAY_PIX_YUV555;
    if (strcmp(name, "6y6uv") == 0) return REPLAY_PIX_6Y6UV;
    if (is_output && strcmp(name, "yuv555-to-6y5uv") == 0)
        return REPLAY_PIX_YUV555_TO_6Y5UV;
    die("unknown %s layout '%s'", is_output ? "output" : "previous", name);
    return REPLAY_PIX_6Y5UV; /* unreachable */
}

int main(int argc, char **argv)
{
    const char *decompressor = NULL, *payload_path = NULL, *size_str = NULL;
    const char *previous_path = NULL, *previous_words16 = NULL;
    const char *output_path = NULL, *output_prefix = NULL;
    const char *payload_prefix = NULL, *output_words_prefix = NULL;
    const char *previous_layout_name = "6y5uv";
    const char *output_layout_name = "6y5uv";
    int codec = 19;
    unsigned frames = 1;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (++i < argc ? argv[i] : (die("%s needs a value", a), ""))
        if (strcmp(a, "--decompressor") == 0) decompressor = NEXT();
        else if (strcmp(a, "--payload") == 0) payload_path = NEXT();
        else if (strcmp(a, "--size") == 0) size_str = NEXT();
        else if (strcmp(a, "--codec") == 0) codec = atoi(NEXT());
        else if (strcmp(a, "--previous") == 0) previous_path = NEXT();
        else if (strcmp(a, "--previous-words16") == 0) previous_words16 = NEXT();
        else if (strcmp(a, "--previous-layout") == 0) previous_layout_name = NEXT();
        else if (strcmp(a, "--output") == 0) output_path = NEXT();
        else if (strcmp(a, "--output-prefix") == 0) output_prefix = NEXT();
        else if (strcmp(a, "--payload-prefix") == 0) payload_prefix = NEXT();
        else if (strcmp(a, "--output-words-prefix") == 0) output_words_prefix = NEXT();
        else if (strcmp(a, "--output-layout") == 0) output_layout_name = NEXT();
        else if (strcmp(a, "--frames") == 0) frames = (unsigned)atoi(NEXT());
        else die("unknown argument '%s'", a);
#undef NEXT
    }

    if (decompressor == NULL) die("--decompressor is required");
    if (payload_path == NULL) die("--payload is required");
    if (size_str == NULL) die("--size is required");
    if (output_path == NULL && output_prefix == NULL)
        die("one of --output or --output-prefix is required");
    if (output_path != NULL && output_prefix != NULL)
        die("--output and --output-prefix are mutually exclusive");
    if (previous_path != NULL && previous_words16 != NULL)
        die("--previous and --previous-words16 are mutually exclusive");
    if (frames == 0) die("--frames must be positive");
    if (frames != 1 && output_path != NULL)
        die("--output can only be used with --frames 1");

    unsigned width = 0, height = 0;
    if (sscanf(size_str, "%ux%u", &width, &height) != 2
        && sscanf(size_str, "%uX%u", &width, &height) != 2)
        die("--size must be WIDTHxHEIGHT");
    if (width == 0 || height == 0) die("--size must be positive");
    size_t pixel_count = (size_t)width * height;

    /* Moving Lines (type 1) exchanges native 16-bit pixels; the YUV codecs use
     * three-byte component triplets in the selected layout. */
    int moving_lines = (codec == 1);
    ReplayPixelLayout out_layout = moving_lines
        ? REPLAY_PIX_RAW16 : layout_from_name(output_layout_name, 1);
    ReplayPixelLayout prev_layout = moving_lines
        ? REPLAY_PIX_RAW16 : layout_from_name(previous_layout_name, 0);

    size_t module_len, payload_len;
    uint8_t *module = read_file(decompressor, &module_len);
    uint8_t *payload = read_file(payload_path, &payload_len);

    char err[256];
    /* The Replay decompressors are 26-bit RISC OS modules (they return with the
     * 26-bit MOVS pc,lr convention), so model a 26-bit ARM. */
    ReplayCodecIf *cif = replay_codecif_open(module, module_len, width, height,
                                             REPLAY_ARM_MODE_26, err, sizeof err);
    if (cif == NULL) die("%s", err);

    size_t frame_words = replay_codecif_frame_words_len(cif);

    /* Build and seed the temporal reference, if any. */
    uint8_t *previous_words = NULL;
    if (previous_words16 != NULL) {
        uint8_t *raw = read_exact(previous_words16, pixel_count * 2,
                                  "16-bit previous frame");
        previous_words = malloc(frame_words);
        if (previous_words == NULL) die("out of memory");
        replay_pix_pack(REPLAY_PIX_RAW16, raw, pixel_count, previous_words, NULL);
        free(raw);
    } else if (previous_path != NULL) {
        size_t bpp = replay_pix_bytes_per_pixel(prev_layout);
        uint8_t *raw = read_exact(previous_path, pixel_count * bpp,
                                  "previous frame");
        size_t bad = 0;
        previous_words = malloc(frame_words);
        if (previous_words == NULL) die("out of memory");
        if (replay_pix_pack(prev_layout, raw, pixel_count, previous_words, &bad)
            != 0)
            die("%s: invalid sample at pixel %zu", previous_path, bad);
        free(raw);
    }
    if (previous_words != NULL
        && replay_codecif_set_previous_words(cif, previous_words, frame_words,
                                             err, sizeof err) != 0)
        die("%s", err);

    if (replay_codecif_load_payload(cif, payload, payload_len, err, sizeof err)
        != 0)
        die("%s", err);

    uint8_t *out_words = malloc(frame_words);
    size_t out_bpp = replay_pix_bytes_per_pixel(out_layout);
    uint8_t *out_bytes = malloc(pixel_count * out_bpp);
    if (out_words == NULL || out_bytes == NULL) die("out of memory");

    size_t offset = 0;
    unsigned f;
    for (f = 0; f < frames; f++) {
        size_t start = offset, consumed = 0;
        char path[1024];

        if (replay_codecif_decode(cif, &offset, out_words, &consumed,
                                  err, sizeof err) != 0)
            die("frame %u: %s", f, err);

        if (replay_pix_unpack(out_layout, out_words, pixel_count, out_bytes) != 0)
            die("frame %u: unpack failed", f);

        if (output_path != NULL) {
            write_file(output_path, out_bytes, pixel_count * out_bpp);
            snprintf(path, sizeof path, "%s", output_path);
        } else {
            numbered_path(path, sizeof path, output_prefix, f, ".6y5uv");
            write_file(path, out_bytes, pixel_count * out_bpp);
        }

        if (output_words_prefix != NULL) {
            char wpath[1024];
            numbered_path(wpath, sizeof wpath, output_words_prefix, f, ".words");
            write_file(wpath, out_words, frame_words);
        }
        if (payload_prefix != NULL) {
            char ppath[1024];
            numbered_path(ppath, sizeof ppath, payload_prefix, f, ".mb19");
            write_file(ppath, payload + start, consumed);
        }

        printf("frame=%u codec=%d source_start=%zu source_end=%zu bytes=%zu "
               "output=\"%s\" status=ok\n",
               f, codec, start, offset, consumed, path);
    }

    printf("decompressor=\"%s\" payload=\"%s\" codec=%d size=%ux%u frames=%u "
           "consumed=%zu trailing=%zu status=ok\n",
           decompressor, payload_path, codec, width, height, frames,
           offset, payload_len - offset);

    free(out_bytes);
    free(out_words);
    free(previous_words);
    free(payload);
    free(module);
    replay_codecif_close(cif);
    return 0;
}
