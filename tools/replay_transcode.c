/* replay-transcode -- decode a Replay (ARMovie) movie to raw RGB24 frames.
 *
 * Parses the container, runs the original compiled decompressor for the movie's
 * codec under the ARMulator (via replay/codecif.h), converts each decoded frame
 * to RGB24, and streams the frames to stdout -- ready to pipe into ffmpeg:
 *
 *   replay-transcode --input movie,ae7 --modules-dir ../!ARMovie_compiled \
 *     | ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS \
 *         -i - out.mp4
 *
 * The exact ffmpeg command (with this movie's geometry and rate) is printed to
 * stderr. The decode loop and pixel conversions are shared with replay-armsim
 * through replay/codecif.h; only the container walk and RGB conversion live here.
 *
 * The compiled decompressor is not stored in the movie, so it must be supplied:
 * either a specific file (--module) or a !ARMovie-style directory (--modules-dir)
 * from which DecompN/Decompress,ffd (or MovingLine/Decompress,ffd) is taken.
 */

#include "replay/codecif.h"
#include "replay/mb_color.h"
#include "replay/mb_frame.h"
#include "replay/replay_ae7.h"
#include "replay/replay_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *prog = "replay-transcode";

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

/* Per-codec facts the transcoder needs. */
typedef struct {
    ReplayPixelLayout layout;
    const char *module_subpath; /* under a !ARMovie-style modules dir */
    const char *name;
} CodecInfo;

static int codec_info(unsigned codec, CodecInfo *out)
{
    switch (codec) {
    case 1:
        out->layout = REPLAY_PIX_RAW16;
        out->module_subpath = "MovingLine/Decompress,ffd";
        out->name = "Moving Lines";
        return 0;
    case 7:
        out->layout = REPLAY_PIX_YUV555;
        out->module_subpath = "Decomp7/Decompress,ffd";
        out->name = "Moving Blocks";
        return 0;
    case 17:
        out->layout = REPLAY_PIX_YUV555;
        out->module_subpath = "Decomp17/Decompress,ffd";
        out->name = "Moving Blocks HQ";
        return 0;
    case 19:
        out->layout = REPLAY_PIX_6Y5UV;
        out->module_subpath = "Decomp19/Decompress,ffd";
        out->name = "Super Moving Blocks";
        return 0;
    case 20:
        out->layout = REPLAY_PIX_6Y6UV;
        out->module_subpath = "Decomp20/Decompress,ffd";
        out->name = "Moving Blocks Beta";
        return 0;
    default:
        return -1;
    }
}

/* Convert one decoded frame (codec working words) to RGB24. */
static void frame_to_rgb24(ReplayPixelLayout layout, const uint8_t *words,
                           MbPixel *pixels, unsigned width, unsigned height,
                           uint8_t *rgb)
{
    size_t count = (size_t)width * height;
    size_t stride = (size_t)width * 3;

    if (layout == REPLAY_PIX_RAW16) {
        /* Moving Lines stores a 15-bit pixel, red in the low bits (R[0:4]
         * G[5:9] B[10:14]); expand each component to eight bits. */
        size_t i;
        for (i = 0; i < count; i++) {
            unsigned lo = words[i * 4];
            unsigned hi = words[i * 4 + 1];
            unsigned v = lo | (hi << 8);
            unsigned r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
            rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[i * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
            rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
        return;
    }

    /* The YUV codecs: unpack to Y,U,V triplets (MbPixel) and use the project's
     * CompLib-derived preview conversion for this working format. */
    replay_pix_unpack(layout, words, count, (uint8_t *)pixels);
    {
        MbFrame frame;
        ReplayStatus st;
        frame.width = width;
        frame.height = height;
        frame.stride = width;
        frame.pixels = pixels;
        switch (layout) {
        case REPLAY_PIX_6Y5UV:
            st = mb_color_6y5uv_to_rgb24(&frame, rgb, stride);
            break;
        case REPLAY_PIX_6Y6UV:
            st = mb_color_6y6uv_to_rgb24(&frame, rgb, stride);
            break;
        case REPLAY_PIX_YUV555:
        default:
            st = mb_color_yuv555_to_rgb24(&frame, rgb, stride);
            break;
        }
        if (st != REPLAY_OK)
            die("colour conversion failed");
    }
}

int main(int argc, char **argv)
{
    const char *input_path = NULL, *module_path = NULL, *modules_dir = NULL;
    const char *output_path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (++i < argc ? argv[i] : (die("%s needs a value", a), ""))
        if (strcmp(a, "--input") == 0) input_path = NEXT();
        else if (strcmp(a, "--module") == 0) module_path = NEXT();
        else if (strcmp(a, "--modules-dir") == 0) modules_dir = NEXT();
        else if (strcmp(a, "--output") == 0) output_path = NEXT();
        else die("unknown argument '%s'", a);
#undef NEXT
    }
    if (input_path == NULL) die("--input MOVIE is required");
    if (module_path == NULL && modules_dir == NULL)
        die("one of --module or --modules-dir is required");

    size_t movie_len;
    uint8_t *movie_data = read_file(input_path, &movie_len);

    ReplayAe7Movie movie;
    char err[256];
    if (replay_ae7_parse(movie_data, movie_len, &movie, err, sizeof err)
        != REPLAY_OK)
        die("%s: %s", input_path, err);

    CodecInfo info;
    if (codec_info(movie.video_codec, &info) != 0)
        die("video codec %u is not yet supported by the transcoder",
            movie.video_codec);

    /* Resolve the decompressor module. */
    char module_buf[1024];
    if (module_path == NULL) {
        snprintf(module_buf, sizeof module_buf, "%s/%s",
                 modules_dir, info.module_subpath);
        module_path = module_buf;
    }
    size_t module_len;
    uint8_t *module = read_file(module_path, &module_len);

    FILE *out = stdout;
    if (output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL)
            die("cannot create %s", output_path);
    }

    ReplayCodecIf *cif = replay_codecif_open(module, module_len,
                                             movie.width, movie.height,
                                             REPLAY_ARM_MODE_26,
                                             err, sizeof err);
    if (cif == NULL) die("%s", err);

    size_t frame_words = replay_codecif_frame_words_len(cif);
    size_t pixel_count = (size_t)movie.width * movie.height;
    uint8_t *out_words = malloc(frame_words);
    MbPixel *pixels = malloc(pixel_count * sizeof(MbPixel));
    uint8_t *rgb = malloc(pixel_count * 3);
    if (out_words == NULL || pixels == NULL || rgb == NULL)
        die("out of memory");

    fprintf(stderr, "%s: codec %u (%s), %ux%u, %.4g fps, %zu chunks x %u frames\n",
            prog, movie.video_codec, info.name, movie.width, movie.height,
            movie.frames_per_second, movie.chunk_count, movie.frames_per_chunk);

    unsigned long total = 0;
    size_t c;
    for (c = 0; c < movie.chunk_count; c++) {
        const ReplayAe7Chunk *chunk = &movie.chunks[c];
        size_t voff = (size_t)chunk->file_offset;
        size_t vbytes = (size_t)chunk->video_bytes;
        unsigned f;

        if (vbytes == 0)
            continue;
        if (voff > movie_len || vbytes > movie_len - voff)
            die("chunk %zu video region is out of range", c);
        if (replay_codecif_load_payload(cif, movie_data + voff, vbytes,
                                        err, sizeof err) != 0)
            die("chunk %zu: %s", c, err);

        size_t offset = 0;
        for (f = 0; f < movie.frames_per_chunk; f++) {
            size_t consumed = 0;
            if (replay_codecif_decode(cif, &offset, out_words, &consumed,
                                      err, sizeof err) != 0) {
                /* The final chunk may be padded with fewer real frames than
                 * frames-per-chunk; treat a clean failure there as end of
                 * video rather than an error. */
                if (c + 1 == movie.chunk_count)
                    break;
                die("chunk %zu frame %u: %s", c, f, err);
            }
            frame_to_rgb24(info.layout, out_words, pixels,
                           movie.width, movie.height, rgb);
            if (fwrite(rgb, 1, pixel_count * 3, out) != pixel_count * 3)
                die("write error");
            total++;
        }
    }

    fprintf(stderr,
            "%s: wrote %lu frames\n"
            "%s: pipe into: ffmpeg -f rawvideo -pixel_format rgb24 "
            "-video_size %ux%u -framerate %.6g -i - -c:v libx264 -pix_fmt "
            "yuv420p out.mp4\n",
            prog, total, prog, movie.width, movie.height,
            movie.frames_per_second);

    if (out != stdout)
        fclose(out);
    free(rgb);
    free(pixels);
    free(out_words);
    replay_codecif_close(cif);
    replay_ae7_movie_destroy(&movie);
    free(module);
    free(movie_data);
    return 0;
}
