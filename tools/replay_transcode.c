/* replay-transcode -- decode a Replay (ARMovie) movie to raw RGB24 video (and
 * optionally a WAV sound track) for ffmpeg.
 *
 * Parses the container, decodes each frame -- running the original compiled
 * decompressor under the ARMulator (via replay/codecif.h) for the Moving
 * Blocks / Moving Lines codecs and for the uncompressed formats (whose tiny
 * Decompress modules just unpack fixed-layout pixels), or with the native
 * unpacker for type 23 -- converts to RGB24, and streams the frames to stdout:
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

static int ci_contains(const char *haystack, const char *needle); /* below */

/* Interpretation of a decompressor's unpatched working-output word. See
 * docs/spec/uncompressed-video-formats.md. */
typedef enum {
    COL_6Y5UV,   /* Y[0:5] U[6:10] V[11:15] */
    COL_6Y6UV,   /* Y[0:5] U[6:11] V[12:17] */
    COL_YUV555,  /* Y[0:4] U[5:9] V[10:14] */
    COL_RGB555,  /* R[0:4] G[5:9] B[10:14], red low */
    COL_YUV888,  /* Y[0:7] U[8:15] V[16:23] (CCIR, best-effort) */
    COL_RGB888,  /* R[0:7] G[8:15] B[16:23] */
    COL_PAL8     /* 8-bit palette index / greyscale */
} VideoColour;

typedef struct {
    const char *module_subpath; /* DecompN/Decompress,ffd; NULL = native type 23 */
    const char *name;
    VideoColour colour;         /* working-output interpretation */
    int header_colour;          /* refine `colour` from the movie's colour label */
    int direct_type23;          /* decode with the native replay_type23 unpacker */
} CodecInfo;

static int codec_info(unsigned codec, CodecInfo *out)
{
    memset(out, 0, sizeof *out);
    switch (codec) {
    /* Compressed (ARM decompressor). */
    case 1: out->module_subpath = "MovingLine/Decompress,ffd";
        out->name = "Moving Lines"; out->colour = COL_RGB555;
        out->header_colour = 1; return 0;
    case 7: out->module_subpath = "Decomp7/Decompress,ffd";
        out->name = "Moving Blocks"; out->colour = COL_YUV555; return 0;
    case 17: out->module_subpath = "Decomp17/Decompress,ffd";
        out->name = "Moving Blocks HQ"; out->colour = COL_YUV555; return 0;
    case 19: out->module_subpath = "Decomp19/Decompress,ffd";
        out->name = "Super Moving Blocks"; out->colour = COL_6Y5UV; return 0;
    case 20: out->module_subpath = "Decomp20/Decompress,ffd";
        out->name = "Moving Blocks Beta"; out->colour = COL_6Y6UV; return 0;
    /* Uncompressed (tiny unpacking decompressor; output per the Info colour
     * line -- see docs/spec/uncompressed-video-formats.md). */
    case 2: out->module_subpath = "Decomp2/Decompress,ffd";
        out->name = "16bpp uncompressed"; out->colour = COL_RGB555;
        out->header_colour = 1; return 0;
    case 3: out->module_subpath = "Decomp3/Decompress,ffd";
        out->name = "YYUV (10bpp)"; out->colour = COL_YUV555; return 0;
    case 4: out->module_subpath = "Decomp4/Decompress,ffd";
        out->name = "8bpp uncompressed"; out->colour = COL_PAL8; return 0;
    case 5: out->module_subpath = "Decomp5/Decompress,ffd";
        out->name = "4Y1UV (8bpp)"; out->colour = COL_YUV555; return 0;
    case 6: out->module_subpath = "Decomp6/Decompress,ffd";
        out->name = "16Y1UV (6bpp)"; out->colour = COL_YUV555; return 0;
    case 8: out->module_subpath = "Decomp8/Decompress,ffd";
        out->name = "24bpp uncompressed"; out->colour = COL_RGB888;
        out->header_colour = 1; return 0;
    case 9: out->module_subpath = "Decomp9/Decompress,ffd";
        out->name = "YYUV8 (16bpp)"; out->colour = COL_YUV888; return 0;
    case 10: out->module_subpath = "Decomp10/Decompress,ffd";
        out->name = "4Y1UV8 (12bpp)"; out->colour = COL_YUV888; return 0;
    case 11: out->module_subpath = "Decomp11/Decompress,ffd";
        out->name = "16Y1UV8 (9bpp)"; out->colour = COL_YUV888; return 0;
    case 16: out->module_subpath = "Decomp16/Decompress,ffd";
        out->name = "4Y1UV8 (12bpp)"; out->colour = COL_YUV888; return 0;
    case 21: out->module_subpath = "Decomp21/Decompress,ffd";
        out->name = "YUYV8 (16bpp)"; out->colour = COL_YUV888; return 0;
    case 22: out->module_subpath = "Decomp22/Decompress,ffd";
        out->name = "YY8UVd4 (12bpp)"; out->colour = COL_YUV888; return 0;
    case 23: out->name = "6Y6Y5U5V (11bpp)"; out->colour = COL_6Y5UV;
        out->direct_type23 = 1; return 0;
    case 24: out->module_subpath = "Decomp24/Decompress,ffd";
        out->name = "4x6Y1x5UV (8.5bpp)"; out->colour = COL_6Y5UV; return 0;
    case 25: out->module_subpath = "Decomp25/Decompress,ffd";
        out->name = "YYYYd4UVd4 (6bpp)"; out->colour = COL_6Y6UV; return 0;
    default: return -1;
    }
}

/* For formats whose colour model is set by the movie header (2, 8, Moving
 * Lines), refine the default from the bits-per-pixel colour label. `base` is
 * the depth-appropriate RGB default (RGB555 at 16bpp, RGB888 at 24bpp). */
static VideoColour refine_colour(VideoColour base, const char *label)
{
    int is24 = (base == COL_RGB888);
    if (ci_contains(label, "6Y5UV")) return COL_6Y5UV;
    if (ci_contains(label, "6Y6UV")) return COL_6Y6UV;
    if (ci_contains(label, "YUV")) return is24 ? COL_YUV888 : COL_YUV555;
    if (ci_contains(label, "RGB")) return is24 ? COL_RGB888 : COL_RGB555;
    return base; /* RGB is the documented default */
}

/* Parse a codec Info file's colour line ("YUV 8,8,8", "6Y5UV 6,5,5",
 * "RGB 5,5,5", or a bare bit-depth like "8" for palette) into a VideoColour.
 * Returns 1 on success. */
static int colour_from_info_line(const char *line, VideoColour *out)
{
    while (*line == ' ')
        line++;
    if (strncmp(line, "6Y5UV", 5) == 0) { *out = COL_6Y5UV; return 1; }
    if (strncmp(line, "6Y6UV", 5) == 0) { *out = COL_6Y6UV; return 1; }
    if (strncmp(line, "YUV", 3) == 0 || strncmp(line, "RGB", 3) == 0) {
        int rgb = (line[0] == 'R');
        const char *p = line + 3;
        const char *comma;
        int a, b;
        while (*p == ' ')
            p++;
        a = atoi(p);
        comma = strchr(p, ',');
        b = comma != NULL ? atoi(comma + 1) : a;
        if (a == 8) { *out = rgb ? COL_RGB888 : COL_YUV888; return 1; }
        if (a == 5) { *out = rgb ? COL_RGB555 : COL_YUV555; return 1; }
        if (a == 6) { *out = (b == 6) ? COL_6Y6UV : COL_6Y5UV; return 1; }
        return 0;
    }
    if (line[0] >= '0' && line[0] <= '9') { *out = COL_PAL8; return 1; }
    return 0;
}

/* Locate a decompressor's Info file (alongside the module, or
 * <modules_dir>/Decomp<codec>/Info) and read its working-output colour model.
 * The colour is line 7 by Acorn convention; fall back to the first colour-like
 * line. *multi is set when the Info lists several models (the movie header then
 * selects). Returns 0 on success. */
static int info_colour_for(unsigned codec, const char *module_path,
                           const char *modules_dir, VideoColour *colour,
                           int *multi)
{
    char info_path[1024];
    char line[256], line7[256] = {0}, first[256] = {0};
    const char *chosen = NULL;
    VideoColour scratch;
    FILE *f;
    int ln = 0;

    if (module_path != NULL) {
        const char *slash = strrchr(module_path, '/');
        int dir_len = slash != NULL ? (int)(slash - module_path + 1) : 0;
        snprintf(info_path, sizeof info_path, "%.*sInfo", dir_len, module_path);
    } else if (modules_dir != NULL) {
        snprintf(info_path, sizeof info_path, "%s/Decomp%u/Info",
                 modules_dir, codec);
    } else {
        return -1;
    }

    f = fopen(info_path, "rb");
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (++ln == 7)
            snprintf(line7, sizeof line7, "%s", line);
        if (first[0] == '\0' && colour_from_info_line(line, &scratch))
            snprintf(first, sizeof first, "%s", line);
    }
    fclose(f);

    if (line7[0] != '\0' && colour_from_info_line(line7, &scratch))
        chosen = line7;
    else if (first[0] != '\0')
        chosen = first;
    if (chosen == NULL)
        return -1;
    *multi = (strchr(chosen, ';') != NULL);
    return colour_from_info_line(chosen, colour) ? 0 : -1;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* YUV 8,8,8 to RGB. Verified against Acorn's own colour maps
 * (ARMovie_2003/Colour/bas/YUV24 and YUV16): chroma is signed (-128..127, the
 * maps sign-extend the byte), luma is full-range 0..255, and the transform is
 * R = Y + 1.40V, G = Y - 0.34U - 0.71V, B = Y + 1.77U (Acorn's effective
 * coefficients are 255/128 * {0.701, 0.114*0.886/0.587, 0.299*0.701/0.587,
 * 0.886} = 1.397/0.343/0.711/1.765). The BT.601 constants below match those to
 * <0.5% and are the same ones the 5/6-bit converters in mb_color use, so all
 * formats share one preview transform. (YUV24 has an apparent /0.576 typo on
 * the green-from-V term; 0.714 here matches the intended 0.587-based value.) */
static void yuv888_to_rgb(int y, int u, int v, uint8_t *out)
{
    out[0] = clamp8(y + (1402 * v) / 1000);
    out[1] = clamp8(y - (344 * u) / 1000 - (714 * v) / 1000);
    out[2] = clamp8(y + (1772 * u) / 1000);
}

/* Convert one decoded frame (working-output words) to RGB24. `pixels` is
 * scratch for the YUV-triplet paths; `palette` is 256*3 RGB or NULL. */
static void convert_frame(VideoColour colour, const uint8_t *words,
                          MbPixel *pixels, unsigned width, unsigned height,
                          const uint8_t *palette, uint8_t *rgb)
{
    size_t count = (size_t)width * height;
    size_t stride = (size_t)width * 3;
    size_t i;

    switch (colour) {
    case COL_6Y5UV:
    case COL_6Y6UV:
    case COL_YUV555: {
        ReplayPixelLayout pl = colour == COL_6Y5UV ? REPLAY_PIX_6Y5UV
            : colour == COL_6Y6UV ? REPLAY_PIX_6Y6UV : REPLAY_PIX_YUV555;
        MbFrame frame;
        ReplayStatus st;
        replay_pix_unpack(pl, words, count, (uint8_t *)pixels);
        frame.width = width;
        frame.height = height;
        frame.stride = width;
        frame.pixels = pixels;
        st = colour == COL_6Y5UV ? mb_color_6y5uv_to_rgb24(&frame, rgb, stride)
            : colour == COL_6Y6UV ? mb_color_6y6uv_to_rgb24(&frame, rgb, stride)
            : mb_color_yuv555_to_rgb24(&frame, rgb, stride);
        if (st != REPLAY_OK)
            die("colour conversion failed");
        break;
    }
    case COL_RGB555:
        for (i = 0; i < count; i++) {
            unsigned v = words[i * 4] | ((unsigned)words[i * 4 + 1] << 8);
            unsigned r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
            rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[i * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
            rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
        break;
    case COL_RGB888:
        for (i = 0; i < count; i++) {
            rgb[i * 3 + 0] = words[i * 4 + 0];
            rgb[i * 3 + 1] = words[i * 4 + 1];
            rgb[i * 3 + 2] = words[i * 4 + 2];
        }
        break;
    case COL_YUV888:
        for (i = 0; i < count; i++)
            yuv888_to_rgb(words[i * 4], (int8_t)words[i * 4 + 1],
                          (int8_t)words[i * 4 + 2], &rgb[i * 3]);
        break;
    case COL_PAL8:
        for (i = 0; i < count; i++) {
            unsigned idx = words[i * 4];
            if (palette != NULL) {
                rgb[i * 3 + 0] = palette[idx * 3 + 0];
                rgb[i * 3 + 1] = palette[idx * 3 + 1];
                rgb[i * 3 + 2] = palette[idx * 3 + 2];
            } else {
                rgb[i * 3 + 0] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = (uint8_t)idx;
            }
        }
        break;
    }
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
        case AUDIO_ADPCM:
            /* Each chunk is self-contained: a per-channel state header (4 bytes
             * each: valprev LE, index, pad) then 4-bit IMA codes. Mono packs
             * two samples per byte; stereo packs one frame per byte (left in
             * the low nibble, right in the high). */
            if (movie->sound_channels == 2) {
                ReplaySoundAdpcmState l, r;
                size_t frames;
                int16_t *out;
                if (sbytes < 8)
                    die("chunk %zu stereo ADPCM region too short", c);
                l.predicted = (int16_t)(s[0] | (s[1] << 8));
                l.step_index = (int8_t)s[2];
                r.predicted = (int16_t)(s[4] | (s[5] << 8));
                r.step_index = (int8_t)s[6];
                frames = sbytes - 8;
                out = malloc(frames * 2 * sizeof *out + 1);
                if (out == NULL)
                    die("out of memory decoding ADPCM");
                replay_sound_adpcm_decode_stereo(s + 8, frames, &l, &r, out);
                for (i = 0; i < frames * 2; i++)
                    append_s16le(pcm, out[i]);
                free(out);
            } else {
                ReplaySoundAdpcmState m;
                size_t samples;
                int16_t *out;
                if (sbytes < 4)
                    die("chunk %zu ADPCM region too short", c);
                m.predicted = (int16_t)(s[0] | (s[1] << 8));
                m.step_index = (int8_t)s[2];
                samples = (sbytes - 4) * 2;
                out = malloc(samples * sizeof *out + 1);
                if (out == NULL)
                    die("out of memory decoding ADPCM");
                replay_sound_adpcm_decode(s + 4, samples, &m, out);
                for (i = 0; i < samples; i++)
                    append_s16le(pcm, out[i]);
                free(out);
            }
            break;
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
    VideoColour colour = info->colour;
    const uint8_t *palette = NULL;

    if (output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL)
            die("cannot create %s", output_path);
    }
    pixels = malloc((pixel_count ? pixel_count : 1) * sizeof(MbPixel));
    rgb = malloc((pixel_count ? pixel_count : 1) * 3);
    if (pixels == NULL || rgb == NULL)
        die("out of memory");

    /* Formats whose colour model is header-driven (2, 8, Moving Lines). */
    if (info->header_colour)
        colour = refine_colour(info->colour, movie->pixel_label);

    /* 8-bit palette: the header carries "palette <offset>" to a 256*3 RGB
     * table. Without one the index is treated as greyscale. */
    if (colour == COL_PAL8) {
        const char *p = strstr(movie->pixel_label, "palette");
        if (p != NULL) {
            unsigned long off = strtoul(p + 7, NULL, 10);
            if (off != 0 && off + 256u * 3u <= movie_len)
                palette = movie_data + off;
        }
    }

    fprintf(stderr, "%s: codec %u (%s), %ux%u, %.4g fps\n",
            prog, movie->video_codec, info->name, movie->width, movie->height,
            movie->frames_per_second);

    if (info->direct_type23 && module_path == NULL) {
        /* Fixed-size packed frames: unpack directly, no ARM decoder. An
         * explicit --module forces the decompressor route instead. */
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
            if (mb_color_6y5uv_to_rgb24(&frame, rgb, (size_t)movie->width * 3)
                != REPLAY_OK)
                die("colour conversion failed");
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
                convert_frame(colour, out_words, pixels,
                              movie->width, movie->height, palette, rgb);
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
    char generic_sub[64];
    int have_video = (codec_info(movie.video_codec, &info) == 0);
    if (!have_video) {
        /* Unknown codec: if a decompressor is available, drive it generically
         * from its Info file so an arbitrary external decompressor just works. */
        VideoColour col;
        int multi = 0;
        if (info_colour_for(movie.video_codec, module_path, modules_dir,
                            &col, &multi) == 0) {
            memset(&info, 0, sizeof info);
            snprintf(generic_sub, sizeof generic_sub,
                     "Decomp%u/Decompress,ffd", movie.video_codec);
            info.module_subpath = generic_sub;
            info.name = "external (Info file)";
            if (multi) {
                info.colour = movie.pixel_depth >= 24 ? COL_RGB888 : COL_RGB555;
                info.header_colour = 1;
            } else {
                info.colour = col;
            }
            have_video = 1;
            fprintf(stderr,
                    "%s: codec %u not built in; using decompressor + Info file\n",
                    prog, movie.video_codec);
        }
    }
    if (!have_video) {
        if (skip_unsupported)
            fprintf(stderr, "%s: skipping video: codec %u is not supported\n",
                    prog, movie.video_codec);
        else
            die("video codec %u is not supported "
                "(supply --module/--modules-dir for an external decompressor, "
                "or --skip-unsupported for audio-only output)",
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
        } else if (aud == AUDIO_UNKNOWN) {
            if (skip_unsupported)
                fprintf(stderr,
                        "%s: skipping audio: unsupported sound codec "
                        "(codec %u '%s' '%s')\n",
                        prog, movie.sound_codec, movie.sound_format_label,
                        movie.sound_precision_label);
            else
                die("unsupported sound codec (codec %u '%s' '%s'); "
                    "use --audio-format or --skip-unsupported",
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
