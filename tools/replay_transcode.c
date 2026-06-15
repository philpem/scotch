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

#include <ctype.h>
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
    AUDIO_NONE,        /* no sound track */
    AUDIO_UNKNOWN,     /* a sound format we have no decoder for */
    AUDIO_ULAW,        /* 8-bit VIDC exponential (Acorn "µ-law"): SoundE8 */
    AUDIO_ALAW,        /* 8-bit G.711 A-law */
    AUDIO_SIGNED_8,    /* SoundS8 */
    AUDIO_UNSIGNED_8,  /* SoundU8 */
    AUDIO_SIGNED_16,   /* SoundS16 */
    AUDIO_ADPCM        /* 4-bit IMA ADPCM: SoundA4 / "2 adpcm" */
} AudioFormat;

/* Case-insensitive substring test (the header labels are free text). */
static int ci_contains(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *h;
    if (nlen == 0)
        return 1;
    for (h = haystack; *h != '\0'; h++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (h[i] == '\0'
                || tolower((unsigned char)h[i]) != tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen)
            return 1;
    }
    return 0;
}

static const char *audio_format_text(AudioFormat f)
{
    switch (f) {
    case AUDIO_ULAW: return "8-bit VIDC u-law";
    case AUDIO_ALAW: return "8-bit A-law";
    case AUDIO_SIGNED_8: return "8-bit signed linear";
    case AUDIO_UNSIGNED_8: return "8-bit unsigned linear";
    case AUDIO_SIGNED_16: return "16-bit signed linear";
    case AUDIO_ADPCM: return "4-bit IMA ADPCM";
    default: return "unknown";
    }
}

static AudioFormat audio_format_from_name(const char *name)
{
    if (!strcmp(name, "ulaw") || !strcmp(name, "vidc-e8")) return AUDIO_ULAW;
    if (!strcmp(name, "alaw")) return AUDIO_ALAW;
    if (!strcmp(name, "signed-8")) return AUDIO_SIGNED_8;
    if (!strcmp(name, "unsigned-8")) return AUDIO_UNSIGNED_8;
    if (!strcmp(name, "signed-16")) return AUDIO_SIGNED_16;
    if (!strcmp(name, "adpcm")) return AUDIO_ADPCM;
    die("unknown --audio-format '%s' "
        "(ulaw|alaw|signed-8|unsigned-8|signed-16|adpcm)", name);
    return AUDIO_NONE;
}

/* Pick a sound decoder from the header labels, following Acorn's "ToUseSound"
 * dispatch: format 1 selects SoundA/S/U/E from the bits-per-sample label
 * (ADPCM/LIN/UNSIGN, else exponential µ-law) and the bit count; format 2 names
 * its decompressor. Returns AUDIO_UNKNOWN for a track we cannot decode. */
static AudioFormat choose_audio_format(const ReplayAe7Movie *movie)
{
    const char *plabel = movie->sound_precision_label;

    if (movie->sound_codec == REPLAY_AE7_SOUND_NONE)
        return AUDIO_NONE;
    if (movie->sound_codec == REPLAY_AE7_SOUND_NAMED)
        return ci_contains(movie->sound_format_label, "adpcm")
            ? AUDIO_ADPCM : AUDIO_UNKNOWN; /* GSM/G72x/MPEG: no decoder */
    if (movie->sound_codec != REPLAY_AE7_SOUND_VIDC_LOG)
        return AUDIO_UNKNOWN;

    if (movie->sound_precision == 4 || ci_contains(plabel, "adpcm"))
        return AUDIO_ADPCM;
    if (movie->sound_precision == 16)
        return AUDIO_SIGNED_16; /* 16-bit is always linear signed */
    if (movie->sound_precision == 8) {
        if (ci_contains(plabel, "alaw") || ci_contains(plabel, "a-law"))
            return AUDIO_ALAW;
        if (ci_contains(plabel, "lin"))
            return ci_contains(plabel, "unsign") ? AUDIO_UNSIGNED_8
                                                 : AUDIO_SIGNED_8;
        return AUDIO_ULAW; /* exponential is the default */
    }
    return AUDIO_UNKNOWN;
}

/* G.711 A-law expansion to signed 16-bit. */
static int alaw_to_s16(uint8_t a)
{
    int seg, t;
    a ^= 0x55u;
    t = (a & 0x0F) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0)
        t += 8;
    else if (seg == 1)
        t += 0x108;
    else {
        t += 0x108;
        t <<= (seg - 1);
    }
    return (a & 0x80) ? t : -t;
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
        case AUDIO_ULAW:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, replay_sound_vidc_e8_to_s16(s[i]));
            break;
        case AUDIO_ALAW:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, alaw_to_s16(s[i]));
            break;
        case AUDIO_SIGNED_8:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, (int)(int8_t)s[i] << 8);
            break;
        case AUDIO_UNSIGNED_8:
            for (i = 0; i < sbytes; i++)
                append_s16le(pcm, ((int)s[i] - 128) << 8);
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
        case AUDIO_UNKNOWN:
        default:
            return;
        }
    }

    /* Reversed stereo (channels label contains "REVER"): swap each L/R pair so
     * the WAV is canonical left,right. */
    if (movie->sound_channels == 2
        && ci_contains(movie->sound_channels_label, "rever")) {
        size_t frames = pcm->size / 4;
        size_t k;
        for (k = 0; k < frames; k++) {
            uint8_t *p = pcm->data + k * 4;
            uint8_t t0 = p[0], t1 = p[1];
            p[0] = p[2]; p[1] = p[3]; p[2] = t0; p[3] = t1;
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

/* Decode all video frames to RGB24 and write them to output_path (or stdout).
 * Returns the number of frames written. */
static unsigned long transcode_video(const ReplayAe7Movie *movie,
                                     const uint8_t *movie_data,
                                     size_t movie_len, const CodecInfo *info,
                                     const char *module_path,
                                     const char *modules_dir,
                                     const char *output_path)
{
    char err[256];
    FILE *out = stdout;
    size_t pixel_count = (size_t)movie->width * movie->height;
    MbPixel *pixels;
    uint8_t *rgb;
    unsigned long total = 0;

    if (output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL)
            die("cannot create %s", output_path);
    }
    pixels = malloc((pixel_count ? pixel_count : 1) * sizeof(MbPixel));
    rgb = malloc((pixel_count ? pixel_count : 1) * 3);
    if (pixels == NULL || rgb == NULL)
        die("out of memory");

    fprintf(stderr, "%s: codec %u (%s), %ux%u, %.4g fps\n",
            prog, movie->video_codec, info->name, movie->width, movie->height,
            movie->frames_per_second);

    if (info->direct_type23) {
        /* Fixed-size packed frames: unpack directly, no ARM decoder. */
        size_t frame_count = 0, fi;
        if (replay_type23_frame_count(movie, &frame_count) != REPLAY_OK)
            die("type 23: bad frame layout");
        for (fi = 0; fi < frame_count; fi++) {
            MbFrame frame;
            frame.width = movie->width;
            frame.height = movie->height;
            frame.stride = movie->width;
            frame.pixels = pixels;
            if (replay_type23_unpack_frame(movie_data, movie_len, movie, fi,
                                           &frame) != REPLAY_OK)
                die("type 23: unpack frame %zu failed", fi);
            mbframe_to_rgb24(info->layout, &frame, rgb,
                             (size_t)movie->width * 3);
            if (fwrite(rgb, 1, pixel_count * 3, out) != pixel_count * 3)
                die("write error");
            total++;
        }
    } else {
        char module_buf[1024];
        size_t module_len;
        uint8_t *module;
        ReplayCodecIf *cif;
        size_t frame_words;
        uint8_t *out_words;
        size_t c;

        if (module_path == NULL) {
            if (modules_dir == NULL)
                die("codec %u needs a decompressor: pass --module or "
                    "--modules-dir", movie->video_codec);
            snprintf(module_buf, sizeof module_buf, "%s/%s",
                     modules_dir, info->module_subpath);
            module_path = module_buf;
        }
        module = read_file(module_path, &module_len);
        cif = replay_codecif_open(module, module_len, movie->width,
                                  movie->height, REPLAY_ARM_MODE_26,
                                  err, sizeof err);
        if (cif == NULL) die("%s", err);
        frame_words = replay_codecif_frame_words_len(cif);
        out_words = malloc(frame_words);
        if (out_words == NULL) die("out of memory");

        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
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

            for (f = 0; f < movie->frames_per_chunk; f++) {
                size_t consumed = 0;
                if (replay_codecif_decode(cif, &offset, out_words, &consumed,
                                          err, sizeof err) != 0) {
                    if (c + 1 == movie->chunk_count)
                        break; /* final chunk may be short / padded */
                    die("chunk %zu frame %u: %s", c, f, err);
                }
                words_to_rgb24(info->layout, out_words, pixels,
                               movie->width, movie->height, rgb);
                if (fwrite(rgb, 1, pixel_count * 3, out) != pixel_count * 3)
                    die("write error");
                total++;
            }
        }
        free(out_words);
        replay_codecif_close(cif);
        free(module);
    }

    if (out != stdout)
        fclose(out);
    free(rgb);
    free(pixels);
    return total;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL, *module_path = NULL, *modules_dir = NULL;
    const char *output_path = NULL, *audio_output = NULL;
    const char *audio_format_name = NULL;
    int skip_unsupported = 0;
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
        else if (strcmp(a, "--skip-unsupported") == 0) skip_unsupported = 1;
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

    /* ---- decide what we can produce; an unrecognised codec is fatal unless
     * --skip-unsupported asks for partial (video-only / audio-only) output. */
    CodecInfo info;
    int have_video = (codec_info(movie.video_codec, &info) == 0);
    if (!have_video) {
        if (skip_unsupported)
            fprintf(stderr, "%s: skipping video: codec %u is not supported\n",
                    prog, movie.video_codec);
        else
            die("video codec %u is not supported "
                "(use --skip-unsupported for audio-only output)",
                movie.video_codec);
    }

    AudioFormat aud = AUDIO_NONE;
    int do_audio = 0;
    if (audio_output != NULL) {
        aud = audio_format_name != NULL
            ? audio_format_from_name(audio_format_name)
            : choose_audio_format(&movie);
        if (aud == AUDIO_NONE) {
            fprintf(stderr,
                    "%s: movie has no sound track; no audio written\n", prog);
        } else if (aud == AUDIO_UNKNOWN
                   || (aud == AUDIO_ADPCM && movie.sound_channels > 1)) {
            const char *why = (aud == AUDIO_ADPCM)
                ? "stereo ADPCM is not supported"
                : "unsupported sound codec";
            if (skip_unsupported)
                fprintf(stderr, "%s: skipping audio: %s (codec %u '%s' '%s')\n",
                        prog, why, movie.sound_codec, movie.sound_format_label,
                        movie.sound_precision_label);
            else
                die("%s (codec %u '%s' '%s'); "
                    "use --audio-format or --skip-unsupported", why,
                    movie.sound_codec, movie.sound_format_label,
                    movie.sound_precision_label);
        } else {
            do_audio = 1;
        }
    }
    if (!have_video && !do_audio)
        die("nothing to output");

    /* ---- audio (decoded first so a streamed WAV is complete before ffmpeg
     * opens it) ---- */
    if (do_audio) {
        ReplayBuffer pcm;
        replay_buffer_init(&pcm);
        decode_audio(&movie, movie_data, movie_len, aud, &pcm);
        write_wav(audio_output, &pcm, movie.sound_rate,
                  movie.sound_channels ? movie.sound_channels : 1u);
        fprintf(stderr, "%s: wrote %s (%s, %u Hz, %u ch, %zu PCM bytes)\n",
                prog, audio_output, audio_format_text(aud), movie.sound_rate,
                movie.sound_channels, pcm.size);
        replay_buffer_free(&pcm);
    }

    /* ---- video ---- */
    unsigned long total = 0;
    if (have_video)
        total = transcode_video(&movie, movie_data, movie_len, &info,
                                module_path, modules_dir, output_path);

    /* ---- summary + ready-to-run ffmpeg command ---- */
    if (have_video && do_audio)
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -i %s -c:v libx264 -pix_fmt yuv420p "
                "-c:a aac -shortest out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second, audio_output);
    else if (have_video)
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -c:v libx264 -pix_fmt yuv420p out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second);
    else if (do_audio)
        fprintf(stderr, "%s: audio-only; encode with: ffmpeg -i %s out.m4a\n",
                prog, audio_output);

    replay_ae7_movie_destroy(&movie);
    free(movie_data);
    return 0;
}
