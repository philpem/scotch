/* replay-transcode -- decode a Replay (ARMovie) movie to raw RGB24 video (and
 * optionally a WAV sound track) for ffmpeg.
 *
 * Parses the container, decodes each frame -- running the original compiled
 * decompressor under the ARMulator (via replay/codecif.h) for the Moving
 * Blocks / Moving Lines codecs, or unpacking directly for the fixed-size type
 * 23 4:2:2 format -- converts to RGB24, and streams the frames to stdout:
 *
 *   replay-transcode --input movie,ae7 --modules-dir ../!ARMovie_compiled \
 *       --audio-output sound.wav \
 *     | ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS \
 *         -i - -i sound.wav -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
 *
 * The exact ffmpeg command (with this movie's geometry/rate and any sound) is
 * printed to stderr. The video decode loop and pixel conversions are shared
 * with replay-armsim through replay/codecif.h.
 *
 * The compiled decompressor is not stored in the movie, so for the codec types
 * that need one it must be supplied: --module FILE, or --modules-dir DIR from
 * which DecompN/Decompress,ffd (or MovingLine/Decompress,ffd) is taken. Type 23
 * needs no module.
 */

#include "replay/codecif.h"
#include "replay/mb_color.h"
#include "replay/mb_frame.h"
#include "replay/replay_ae7.h"
#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_sound.h"
#include "replay/replay_status.h"
#include "replay/replay_type23.h"

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

/* ------------------------------------------------------------------ */
/* Video                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    ReplayPixelLayout layout;
    const char *module_subpath; /* NULL = no ARM module (type 23) */
    const char *name;
    int direct_type23;
} CodecInfo;

static int codec_info(unsigned codec, CodecInfo *out)
{
    memset(out, 0, sizeof *out);
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
    case 23:
        out->layout = REPLAY_PIX_6Y5UV; /* unpacked to one 6Y5UV sample/pixel */
        out->module_subpath = NULL;
        out->name = "type 23 (6Y6Y5U5V 4:2:2)";
        out->direct_type23 = 1;
        return 0;
    default:
        return -1;
    }
}

static void mbframe_to_rgb24(ReplayPixelLayout layout, const MbFrame *frame,
                             uint8_t *rgb, size_t stride);

/* Convert one decoded frame (codec working words) to RGB24. */
static void words_to_rgb24(ReplayPixelLayout layout, const uint8_t *words,
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

    replay_pix_unpack(layout, words, count, (uint8_t *)pixels);
    {
        MbFrame frame;
        frame.width = width;
        frame.height = height;
        frame.stride = width;
        frame.pixels = pixels;
        mbframe_to_rgb24(layout, &frame, rgb, stride);
    }
}

/* Convert an already-unpacked MbFrame (Y,U,V samples) to RGB24. */
static void mbframe_to_rgb24(ReplayPixelLayout layout, const MbFrame *frame,
                             uint8_t *rgb, size_t stride)
{
    ReplayStatus st;
    switch (layout) {
    case REPLAY_PIX_6Y6UV:
        st = mb_color_6y6uv_to_rgb24(frame, rgb, stride);
        break;
    case REPLAY_PIX_YUV555:
        st = mb_color_yuv555_to_rgb24(frame, rgb, stride);
        break;
    case REPLAY_PIX_6Y5UV:
    default:
        st = mb_color_6y5uv_to_rgb24(frame, rgb, stride);
        break;
    }
    if (st != REPLAY_OK)
        die("colour conversion failed");
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    AUDIO_NONE,
    AUDIO_VIDC_E8,
    AUDIO_SIGNED_8,
    AUDIO_SIGNED_16,
    AUDIO_ADPCM
} AudioFormat;

static AudioFormat audio_format_from_name(const char *name)
{
    if (strcmp(name, "vidc-e8") == 0) return AUDIO_VIDC_E8;
    if (strcmp(name, "signed-8") == 0) return AUDIO_SIGNED_8;
    if (strcmp(name, "signed-16") == 0) return AUDIO_SIGNED_16;
    if (strcmp(name, "adpcm") == 0) return AUDIO_ADPCM;
    die("unknown --audio-format '%s' (vidc-e8|signed-8|signed-16|adpcm)", name);
    return AUDIO_NONE;
}

/* Pick a sound decoder from the header. The VIDC-exponential vs linear
 * distinction lives in a text label the parser does not keep, so 8-bit format-1
 * sound defaults to VIDC-exponential (the classic Replay format); use
 * --audio-format to override. Format 2 ("named": adpcm/GSM/...) is ambiguous
 * without the name, so it requires --audio-format. */
static AudioFormat choose_audio_format(const ReplayAe7Movie *movie)
{
    if (movie->sound_codec == REPLAY_AE7_SOUND_NONE)
        return AUDIO_NONE;
    if (movie->sound_codec == REPLAY_AE7_SOUND_VIDC_LOG) {
        if (movie->sound_precision == 16)
            return AUDIO_SIGNED_16;
        if (movie->sound_precision == 8)
            return AUDIO_VIDC_E8;
    }
    return AUDIO_NONE; /* unknown / needs --audio-format */
}

static void put_u16le(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void append_s16le(ReplayBuffer *buf, int sample)
{
    uint8_t b[2];
    put_u16le(b, (unsigned)(sample & 0xFFFF));
    if (replay_buffer_append(buf, b, 2) != REPLAY_OK)
        die("out of memory building sound track");
}

/* Decode every chunk's sound region into interleaved signed-16 LE PCM. */
static void decode_audio(const ReplayAe7Movie *movie, const uint8_t *data,
                         size_t data_len, AudioFormat format,
                         ReplayBuffer *pcm)
{
    size_t c;
    ReplaySoundAdpcmState adpcm = {0, 0};

    for (c = 0; c < movie->chunk_count; c++) {
        const ReplayAe7Chunk *chunk = &movie->chunks[c];
        size_t soff = (size_t)(chunk->file_offset + chunk->video_bytes);
        size_t sbytes = (size_t)chunk->sound_bytes;
        const uint8_t *s;
        size_t i;

        if (sbytes == 0)
            continue;
        if (soff > data_len || sbytes > data_len - soff)
            die("chunk %zu sound region is out of range", c);
        s = data + soff;

        switch (format) {
        case AUDIO_VIDC_E8:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, replay_sound_vidc_e8_to_s16(s[i]));
            break;
        case AUDIO_SIGNED_8:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, (int)(int8_t)s[i] << 8);
            break;
        case AUDIO_SIGNED_16:
            for (i = 0; i + 1 < sbytes; i += 2)
                append_s16le(pcm, (int16_t)(s[i] | (s[i + 1] << 8)));
            break;
        case AUDIO_ADPCM: {
            /* Mono only: 4-byte per-chunk state header (valprev LE, index,
             * pad), then 4-bit codes, two samples per byte. */
            size_t samples;
            int16_t *out;
            if (movie->sound_channels > 1)
                die("stereo ADPCM sound is not yet supported");
            if (sbytes < 4)
                die("chunk %zu ADPCM region too short", c);
            adpcm.predicted = (int16_t)(s[0] | (s[1] << 8));
            adpcm.step_index = (int8_t)s[2];
            samples = (sbytes - 4) * 2;
            out = malloc(samples * sizeof *out + 1);
            if (out == NULL)
                die("out of memory decoding ADPCM");
            replay_sound_adpcm_decode(s + 4, samples, &adpcm, out);
            for (i = 0; i < samples; i++)
                append_s16le(pcm, out[i]);
            free(out);
            break;
        }
        case AUDIO_NONE:
        default:
            return;
        }
    }
}

static void write_wav(const char *path, const ReplayBuffer *pcm,
                      unsigned rate, unsigned channels)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[44];
    uint32_t data_size = (uint32_t)pcm->size;
    uint32_t byte_rate = rate * channels * 2u;
    if (f == NULL)
        die("cannot create %s", path);
    memcpy(hdr + 0, "RIFF", 4);
    hdr[4] = (uint8_t)(36u + data_size);
    hdr[5] = (uint8_t)((36u + data_size) >> 8);
    hdr[6] = (uint8_t)((36u + data_size) >> 16);
    hdr[7] = (uint8_t)((36u + data_size) >> 24);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;  /* fmt size */
    hdr[20] = 1; hdr[21] = 0;                             /* PCM */
    put_u16le(hdr + 22, channels);
    hdr[24] = (uint8_t)rate; hdr[25] = (uint8_t)(rate >> 8);
    hdr[26] = (uint8_t)(rate >> 16); hdr[27] = (uint8_t)(rate >> 24);
    hdr[28] = (uint8_t)byte_rate; hdr[29] = (uint8_t)(byte_rate >> 8);
    hdr[30] = (uint8_t)(byte_rate >> 16); hdr[31] = (uint8_t)(byte_rate >> 24);
    put_u16le(hdr + 32, channels * 2u);                  /* block align */
    put_u16le(hdr + 34, 16);                             /* bits/sample */
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)data_size; hdr[41] = (uint8_t)(data_size >> 8);
    hdr[42] = (uint8_t)(data_size >> 16); hdr[43] = (uint8_t)(data_size >> 24);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr
        || (pcm->size > 0 && fwrite(pcm->data, 1, pcm->size, f) != pcm->size))
        die("write error on %s", path);
    fclose(f);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *input_path = NULL, *module_path = NULL, *modules_dir = NULL;
    const char *output_path = NULL, *audio_output = NULL;
    const char *audio_format_name = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (++i < argc ? argv[i] : (die("%s needs a value", a), ""))
        if (strcmp(a, "--input") == 0) input_path = NEXT();
        else if (strcmp(a, "--module") == 0) module_path = NEXT();
        else if (strcmp(a, "--modules-dir") == 0) modules_dir = NEXT();
        else if (strcmp(a, "--output") == 0) output_path = NEXT();
        else if (strcmp(a, "--audio-output") == 0) audio_output = NEXT();
        else if (strcmp(a, "--audio-format") == 0) audio_format_name = NEXT();
        else die("unknown argument '%s'", a);
#undef NEXT
    }
    if (input_path == NULL) die("--input MOVIE is required");

    size_t movie_len;
    uint8_t *movie_data = read_file(input_path, &movie_len);

    ReplayAe7Movie movie;
    char err[256];
    if (replay_ae7_parse(movie_data, movie_len, &movie, err, sizeof err)
        != REPLAY_OK)
        die("%s: %s", input_path, err);

    CodecInfo info;
    if (codec_info(movie.video_codec, &info) != 0)
        die("video codec %u is not supported by the transcoder",
            movie.video_codec);

    /* ---- audio (decoded first so a streamed WAV is complete before ffmpeg
     * opens it) ---- */
    AudioFormat aud = AUDIO_NONE;
    if (audio_output != NULL) {
        aud = audio_format_name != NULL
            ? audio_format_from_name(audio_format_name)
            : choose_audio_format(&movie);
        if (aud == AUDIO_NONE)
            die("could not determine sound format (codec %u, %u-bit); "
                "pass --audio-format", movie.sound_codec,
                movie.sound_precision);
        ReplayBuffer pcm;
        replay_buffer_init(&pcm);
        decode_audio(&movie, movie_data, movie_len, aud, &pcm);
        write_wav(audio_output, &pcm, movie.sound_rate,
                  movie.sound_channels ? movie.sound_channels : 1u);
        fprintf(stderr, "%s: wrote %s (%zu PCM bytes, %u Hz, %u ch)\n",
                prog, audio_output, pcm.size, movie.sound_rate,
                movie.sound_channels);
        replay_buffer_free(&pcm);
    }

    /* ---- video ---- */
    FILE *out = stdout;
    if (output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL)
            die("cannot create %s", output_path);
    }

    size_t pixel_count = (size_t)movie.width * movie.height;
    MbPixel *pixels = malloc((pixel_count ? pixel_count : 1) * sizeof(MbPixel));
    uint8_t *rgb = malloc((pixel_count ? pixel_count : 1) * 3);
    if (pixels == NULL || rgb == NULL)
        die("out of memory");

    fprintf(stderr, "%s: codec %u (%s), %ux%u, %.4g fps\n",
            prog, movie.video_codec, info.name, movie.width, movie.height,
            movie.frames_per_second);

    unsigned long total = 0;

    if (info.direct_type23) {
        /* Fixed-size packed frames: unpack directly, no ARM decoder. */
        size_t frame_count = 0, fi;
        if (replay_type23_frame_count(&movie, &frame_count) != REPLAY_OK)
            die("type 23: bad frame layout");
        for (fi = 0; fi < frame_count; fi++) {
            MbFrame frame;
            frame.width = movie.width;
            frame.height = movie.height;
            frame.stride = movie.width;
            frame.pixels = pixels;
            if (replay_type23_unpack_frame(movie_data, movie_len, &movie, fi,
                                           &frame) != REPLAY_OK)
                die("type 23: unpack frame %zu failed", fi);
            mbframe_to_rgb24(info.layout, &frame, rgb, (size_t)movie.width * 3);
            if (fwrite(rgb, 1, pixel_count * 3, out) != pixel_count * 3)
                die("write error");
            total++;
        }
    } else {
        char module_buf[1024];
        if (module_path == NULL) {
            if (modules_dir == NULL)
                die("codec %u needs a decompressor: pass --module or "
                    "--modules-dir", movie.video_codec);
            snprintf(module_buf, sizeof module_buf, "%s/%s",
                     modules_dir, info.module_subpath);
            module_path = module_buf;
        }
        size_t module_len;
        uint8_t *module = read_file(module_path, &module_len);

        ReplayCodecIf *cif = replay_codecif_open(module, module_len,
                                                 movie.width, movie.height,
                                                 REPLAY_ARM_MODE_26,
                                                 err, sizeof err);
        if (cif == NULL) die("%s", err);

        size_t frame_words = replay_codecif_frame_words_len(cif);
        uint8_t *out_words = malloc(frame_words);
        if (out_words == NULL) die("out of memory");

        size_t c;
        for (c = 0; c < movie.chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie.chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            size_t offset = 0;
            unsigned f;

            if (vbytes == 0)
                continue;
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);
            if (replay_codecif_load_payload(cif, movie_data + voff, vbytes,
                                            err, sizeof err) != 0)
                die("chunk %zu: %s", c, err);

            for (f = 0; f < movie.frames_per_chunk; f++) {
                size_t consumed = 0;
                if (replay_codecif_decode(cif, &offset, out_words, &consumed,
                                          err, sizeof err) != 0) {
                    if (c + 1 == movie.chunk_count)
                        break; /* final chunk may be short / padded */
                    die("chunk %zu frame %u: %s", c, f, err);
                }
                words_to_rgb24(info.layout, out_words, pixels,
                               movie.width, movie.height, rgb);
                if (fwrite(rgb, 1, pixel_count * 3, out) != pixel_count * 3)
                    die("write error");
                total++;
            }
        }
        free(out_words);
        replay_codecif_close(cif);
        free(module);
    }

    /* ---- summary + ready-to-run ffmpeg command ---- */
    if (audio_output != NULL)
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -i %s -c:v libx264 -pix_fmt yuv420p "
                "-c:a aac -shortest out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second, audio_output);
    else
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -c:v libx264 -pix_fmt yuv420p out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second);

    if (out != stdout)
        fclose(out);
    free(rgb);
    free(pixels);
    replay_ae7_movie_destroy(&movie);
    free(movie_data);
    return 0;
}
