/* replay-transcode -- decode a Replay (ARMovie) movie to video for ffmpeg.
 *
 * Parses the container, decodes each frame -- running the original compiled
 * decompressor under the ARMulator (via replay/codecif.h) for the Moving
 * Blocks / Moving Lines codecs and for the uncompressed formats (whose tiny
 * Decompress modules just unpack fixed-layout pixels), the native unpacker for
 * type 23, or the native TCA decoder for type 500 -- and muxes the video, sound
 * and geometry/frame-rate into a single NUT stream that pipes straight into
 * ffmpeg (the default):
 *
 *   replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
 *     | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
 *
 * `--output-format raw` is the older split form: headerless RGB24 frames to
 * stdout plus an optional `--audio-output FILE.wav` sidecar (it then prints the
 * exact `-f rawvideo ...` ffmpeg command to stderr). Raw mode cannot represent
 * the pass-through codecs (Indeo, CRAM16, ...), which only ffmpeg can decode --
 * those require NUT. The decode loop and pixel conversions are shared with
 * replay-armsim through replay/codecif.h.
 *
 * The compiled decompressor is not stored in the movie, so for the codec types
 * that need one it must be supplied: --module FILE, or --modules-dir DIR from
 * which DecompN/Decompress,ffd (or MovingLine/Decompress,ffd) is taken. Type 23
 * needs no module.
 *
 * Video format 0 is "no video track" (a sound-only movie): it is decoded for
 * its audio only, never as a codec, so --modules-dir does not make it chase a
 * non-existent Decomp0. Some real sound-only movies carry non-zero dimensions
 * in the header, so this is keyed on the format number, not the geometry.
 */

#include "replay/codecif.h"
#include "replay/mb_color.h"
#include "replay/mb_frame.h"
#include "replay/replay_ae7.h"
#include "replay/replay_ae7_write.h"
#include "replay/replay_buffer.h"
#include "replay/replay_escape100.h"
#include "replay/replay_escape122.h"
#include "replay/replay_escape124.h"
#include "replay/replay_escape130.h"
#include "replay/replay_escape_adpcm.h"
#include "replay/replay_moviefs.h"
#include "replay/replay_nut.h"
#include "replay/replay_sound.h"
#include "replay/replay_status.h"
#include "replay/replay_tca.h"
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
    int direct_tca;             /* decode with the native replay_tca decoder (500) */
    int direct_esc100;          /* decode with the native escape100 decoder (100/102) */
    int direct_esc122;          /* decode with the native escape122 decoder (122) */
    int direct_esc124;          /* decode with the native escape124 decoder (124) */
    int direct_esc130;          /* decode with the native escape130 decoder (130) */
    int moviefs_wrapper;        /* strip MovieFS's 16-byte per-frame header (6xx) */
    int arm_mode_32;            /* run the module in 32-bit ARM mode (WSS codecs) */
    /* Pass-through: instead of decoding, mux each (de-wrapped) codec frame into
     * NUT under this fourcc and let ffmpeg decode it. Set for codecs we can't run
     * in the sandbox (C codecs / screen-painters), e.g. Indeo. NULL = decode. */
    const char *passthrough_fourcc;
    ReplayWrapKind passthrough_wrap; /* per-frame wrapper flavour for the frames */
    int packed8;                /* COL_PAL8 output is 1 byte/pixel (Dec8), not a word */
    int exact_size;             /* decode at the declared size, no block rounding
                                 * (the Info "step" field is an alignment hint, not
                                 * a frame-padding requirement -- see LinePack/800) */
    int moviefs_palette;        /* CRAM8: chunk starts with 0xffffffff + a 256-word
                                 * RGB555 palette, then the wrapper chain */
    int bottom_up;              /* decoder writes rows bottom-up (AVI-sourced) */
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
    /* MovieFS (Warm Silence Software) re-encapsulated PC codecs, 600-699. Each
     * codec frame is wrapped in a 16-byte MovieFS header; the codecs ship
     * several decompressor variants and we deliberately drive the Dec24 variant,
     * whose `FNplook` is empty so it does the full YUV->RGB conversion with its
     * own internal tables and never needs the caller-supplied colour-lookup
     * table (R3) that the screen-painting `Decompress`/`DecompresH` variants
     * require (those infinite-loop unpatched). Dec24 emits 24bpp RGB words
     * (00 BB GG RR), i.e. COL_RGB888. The WSS modules are 32-bit code, so run
     * them in 32-bit ARM mode. See docs/moviefs-nut-passthrough.md. */
    case 602: out->module_subpath = "Decomp602/Dec24,ffd";
        out->name = "Cinepak (CVID, MovieFS)"; out->colour = COL_RGB888;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; return 0;
    case 608: out->module_subpath = "Decomp608/Dec24,ffd";
        out->name = "RGB24 AVI (MovieFS)"; out->colour = COL_RGB888;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; return 0;
    case 615: out->module_subpath = "Decomp615/Dec24,ffd";
        out->name = "QT RLE 24 (MovieFS)"; out->colour = COL_RGB888;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; return 0;
    case 626: out->module_subpath = "Decomp626/Dec24,ffd";
        out->name = "RGB24 QT (MovieFS)"; out->colour = COL_RGB888;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; return 0;
    /* MovieFS palettised codecs via the Dec8 variant: it does no colour
     * transform (patch table is -1, so it is r3-free like Dec24) and emits
     * packed 8-bit palette indices (1 byte/pixel). The palette is the movie's,
     * read from the AE7 header `palette <offset>` (COL_PAL8, the standard Replay
     * 8bpp mechanism). DL (622) and ANM (623) take no palette in their Dec8, so
     * they use the header palette like the rest. FLIC (610) is handled by
     * pass-through instead (its Dec8 keeps a per-frame palette in its workspace;
     * ffmpeg's flic reads the in-stream palette directly). Not yet validated
     * against a sample. */
    case 600: out->module_subpath = "Decomp600/Dec8,ffd";
        out->name = "CRAM8 AVI (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->moviefs_palette = 1; out->bottom_up = 1;
        out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 604: out->module_subpath = "Decomp604/Dec8,ffd";
        out->name = "SMC QT (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 606: out->module_subpath = "Decomp606/Dec8,ffd";
        out->name = "RGB8 AVI (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 607: out->module_subpath = "Decomp607/Dec8,ffd";
        out->name = "RLE8 AVI (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 609: out->module_subpath = "Decomp609/Dec8,ffd";
        out->name = "RLE8 QT (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 613: out->module_subpath = "Decomp613/Dec8,ffd";
        out->name = "RLE4 QT (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 624: out->module_subpath = "Decomp624/Dec8,ffd";
        out->name = "RGB8 QT (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 622: out->module_subpath = "Decomp622/Dec8,ffd";
        out->name = "DL animation (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    case 623: out->module_subpath = "Decomp623/Dec8,ffd";
        out->name = "ANM film (MovieFS)"; out->colour = COL_PAL8;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; out->packed8 = 1; return 0;
    /* MovieFS 16bpp codecs that ship only a screen-painter `Decompress`, but
     * whose `Decompress` is r3-free (no colour-table use, unlike Cinepak's
     * dithering painter): run unpatched, emitting native RGB555 words. 614
     * QT-RLE16 lands here because qtrle's depth can't be carried through NUT, so
     * pass-through to ffmpeg is not an option. (601 CRAM16 and 603 RPZA are also
     * r3-free but go via pass-through below, which ffmpeg handles cleanly.) */
    case 614: out->module_subpath = "Decomp614/Decompress,ffd";
        out->name = "QT RLE 16 (MovieFS)"; out->colour = COL_RGB555;
        out->moviefs_wrapper = 1; out->arm_mode_32 = 1; return 0;
    /* Pass-through to ffmpeg (requires --output-format nut): strip the per-frame
     * wrapper and mux each frame under the matching NUT fourcc, letting ffmpeg
     * decode. Used where ffmpeg is the cleaner path -- codecs whose only sandbox
     * variant needs the colour table (601/603) or a runtime the sandbox lacks
     * (628/629 Indeo, 901/902 VideoFS C codecs), where avoiding ARM emulation is
     * simply preferable (605), or where the codec's own palette lives in the
     * bitstream and ffmpeg already tracks it (610 FLIC, whose Dec8 keeps a
     * per-frame palette in its workspace -- ffmpeg's flic reads the FLI_COLOR
     * chunks itself, no extradata needed). ffmpeg decodes msvideo1 (CRAM), rpza,
     * ulti and flic with no extra metadata; CRAM defaults to 16-bit, matching
     * CRAM16. 6xx are MovieFS-wrapped; 9xx are VideoFS-wrapped (different size
     * field). See docs/moviefs-nut-passthrough.md. Apart from the FLIC decode
     * path (validated with a synthesised frame) these await real samples. */
    case 601: out->name = "CRAM16 AVI (msvideo1, MovieFS, pass-through)";
        out->passthrough_fourcc = "CRAM";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 603: out->name = "RPZA QT (MovieFS, pass-through)";
        out->passthrough_fourcc = "rpza";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 605: out->name = "Ultimotion AVI (ulti, MovieFS, pass-through)";
        out->passthrough_fourcc = "ULTI";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 610: out->name = "FLI/FLC (flic, MovieFS, pass-through)";
        out->passthrough_fourcc = "FLIC";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 628: out->name = "Indeo 3.1 (IV31, MovieFS, pass-through)";
        out->passthrough_fourcc = "IV31";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 629: out->name = "Indeo 3.2 (IV32, MovieFS, pass-through)";
        out->passthrough_fourcc = "IV32";
        out->passthrough_wrap = REPLAY_WRAP_MOVIEFS; return 0;
    case 901: out->name = "Indeo Raw YVU9 (VideoFS, pass-through)";
        out->passthrough_fourcc = "YVU9";
        out->passthrough_wrap = REPLAY_WRAP_VIDEOFS; return 0;
    case 902: out->name = "Indeo 3.2 (IV32, VideoFS, pass-through)";
        out->passthrough_fourcc = "IV32";
        out->passthrough_wrap = REPLAY_WRAP_VIDEOFS; return 0;
    /* Eidos "Escape 2.0" (video format 130) -- a YCbCr 2x2-block codec, decoded
     * natively by `replay_esc130` (clean-room decode + a bit-exact reimplementation
     * of DEC130.DLL's render). One frame per chunk. See docs/spec/eidos-escape.md. */
    case 130: out->name = "Eidos Escape 2.0 (escape130)"; out->colour = COL_RGB888;
        out->direct_esc130 = 1; return 0;
    /* Eidos "Escape 122" -- a palettised (PAL8) codec, unrelated to escape124/130
     * (the Streamer DLLs can't decode it); decoded natively by `replay_esc122`
     * from the format spec in docs/spec/eidos-escape.md. */
    /* Eidos "Escape" 100/102 (© Eidos 1993) -- a 5-bit-YUV 2x2-block VQ codec,
     * decoded natively by `replay_esc100` (RE'd from the Decomp100/102 modules).
     * A single video chunk concatenates several frames. See docs/spec/eidos-escape.md. */
    case 100: out->name = "Eidos Escape 100 (YUV555)"; out->colour = COL_YUV555;
        out->direct_esc100 = 1; return 0;
    case 102: out->name = "Eidos Escape 102 (YUV555)"; out->colour = COL_YUV555;
        out->direct_esc100 = 1; return 0;
    case 122: out->name = "Eidos Escape 122 (PAL8)"; out->colour = COL_PAL8;
        out->direct_esc122 = 1; return 0;
    /* Eidos "Escape" games codec 124 -- an RGB555 block codec (WINSDEC/EDEC),
     * decoded natively by `replay_esc124`. A single video chunk concatenates
     * `frames_per_chunk` frames. See docs/spec/eidos-escape.md (§ Type 124). */
    case 124: out->name = "Eidos Escape 124 (RGB555)"; out->colour = COL_RGB555;
        out->direct_esc124 = 1; return 0;
    /* Iota "The Complete Animator" (TCA/ACEF) — decoded natively by replay_tca
     * (the film is embedded in the Replay container); emits 8bpp + its own PALE
     * palette. See docs/spec/tca-type500.md. */
    case 500: out->name = "Complete Animator (TCA)"; out->colour = COL_PAL8;
        out->direct_tca = 1; return 0;
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
    /* LinePack (Henrik Bjerregaard Pedersen, 1995): a third-party temporal/spatial
     * codec emitting 15-bit RGB/YUV words (its `Decompress` FNplook is an unpatched
     * passthrough). It decodes at the *exact* declared size: its Info "step" of 32
     * is an alignment hint, not a frame-padding requirement (TEKTRAILER is 160x120,
     * not a multiple of 32). Block-rounding to 160x128 over-fills each frame and
     * desyncs the source. See docs/spec/linepack-type800.md. */
    case 800: out->module_subpath = "Decomp800/Decompress,ffd";
        out->name = "LinePack"; out->colour = COL_RGB555;
        out->header_colour = 1; out->exact_size = 1; return 0;
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

/* Read the codec's human name from Info line 1 (e.g. "Escape") into `name`
 * (up to `n` bytes). Used to label an unknown, externally decompressed codec
 * instead of a bare "external". Returns 0 on success. */
static int info_name_for(unsigned codec, const char *module_path,
                         const char *modules_dir, char *name, size_t n)
{
    char info_path[1024];
    FILE *f;

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
    if (fgets(name, (int)n, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    return name[0] != '\0' ? 0 : -1;
}

/* A genuine block-rounding mask is a power of two in [1,64] (every real codec
 * uses 1,2,4,8,16 or 32). Escape's Info puts maximum dimensions on lines 4/5
 * (160/128), which the rounding logic would misread as a block, so reject
 * anything that isn't a sane power-of-two block and fall back to 1 (no
 * rounding) — exactly what Escape needs, and what the buggy 2003 Player fails
 * to do. */
static unsigned valid_block(long v)
{
    if (v >= 1 && v <= 64 && (v & (v - 1)) == 0)
        return (unsigned)v;
    return 1;
}

static unsigned block_round_up(unsigned dim, unsigned block)
{
    return (dim + block - 1) & ~(block - 1); /* block is a power of two */
}

/* Read the codec's pixel-block size from Info lines 4 and 5 (the field after
 * the first ';', as the RISC OS Player does). The decoder must run at
 * dimensions rounded up to this block; without it a movie whose size is not a
 * block multiple (e.g. a 24x24 LinePack movie, block 32) decodes desynced.
 * Defaults to 1x1 (no rounding) when no/invalid Info is present. */
static void info_block_for(unsigned codec, const char *module_path,
                           const char *modules_dir,
                           unsigned *block_x, unsigned *block_y)
{
    char info_path[1024], line[256];
    FILE *f;
    int ln = 0;

    *block_x = *block_y = 1;

    if (module_path != NULL) {
        const char *slash = strrchr(module_path, '/');
        int dir_len = slash != NULL ? (int)(slash - module_path + 1) : 0;
        snprintf(info_path, sizeof info_path, "%.*sInfo", dir_len, module_path);
    } else if (modules_dir != NULL) {
        snprintf(info_path, sizeof info_path, "%s/Decomp%u/Info",
                 modules_dir, codec);
    } else {
        return;
    }

    f = fopen(info_path, "rb");
    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL) {
        const char *semi;
        if (++ln != 4 && ln != 5)
            continue;
        semi = strchr(line, ';');
        if (semi != NULL) {
            unsigned b = valid_block(strtol(semi + 1, NULL, 10));
            if (ln == 4)
                *block_x = b;
            else
                *block_y = b;
        }
    }
    fclose(f);
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
/* `words` is the decoder's working output for a frame of `stride` pixels per
 * row (the block-rounded width); we emit only the declared out_w x out_h
 * region, so a block codec run at a rounded size is cropped back here. */
static void convert_frame(VideoColour colour, const uint8_t *words,
                          MbPixel *pixels, unsigned stride,
                          unsigned out_w, unsigned out_h,
                          const uint8_t *palette, int packed8, int bottom_up,
                          uint8_t *rgb)
{
    size_t out_stride = (size_t)out_w * 3;
    unsigned r, x;

    switch (colour) {
    case COL_6Y5UV:
    case COL_6Y6UV:
    case COL_YUV555: {
        ReplayPixelLayout pl = colour == COL_6Y5UV ? REPLAY_PIX_6Y5UV
            : colour == COL_6Y6UV ? REPLAY_PIX_6Y6UV : REPLAY_PIX_YUV555;
        MbFrame frame;
        ReplayStatus st;
        replay_pix_unpack(pl, words, (size_t)stride * out_h, (uint8_t *)pixels);
        frame.width = out_w;
        frame.height = out_h;
        frame.stride = stride;
        frame.pixels = pixels;
        st = colour == COL_6Y5UV
                 ? mb_color_6y5uv_to_rgb24(&frame, rgb, out_stride)
             : colour == COL_6Y6UV
                 ? mb_color_6y6uv_to_rgb24(&frame, rgb, out_stride)
                 : mb_color_yuv555_to_rgb24(&frame, rgb, out_stride);
        if (st != REPLAY_OK)
            die("colour conversion failed");
        break;
    }
    case COL_RGB555:
        for (r = 0; r < out_h; r++)
            for (x = 0; x < out_w; x++) {
                size_t s = (size_t)(bottom_up ? out_h - 1 - r : r) * stride + x,
                       d = (size_t)r * out_w + x;
                unsigned v = words[s * 4] | ((unsigned)words[s * 4 + 1] << 8);
                unsigned rr = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
                rgb[d * 3 + 0] = (uint8_t)((rr << 3) | (rr >> 2));
                rgb[d * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
                rgb[d * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
            }
        break;
    case COL_RGB888:
        for (r = 0; r < out_h; r++)
            for (x = 0; x < out_w; x++) {
                size_t s = (size_t)(bottom_up ? out_h - 1 - r : r) * stride + x,
                       d = (size_t)r * out_w + x;
                rgb[d * 3 + 0] = words[s * 4 + 0];
                rgb[d * 3 + 1] = words[s * 4 + 1];
                rgb[d * 3 + 2] = words[s * 4 + 2];
            }
        break;
    case COL_YUV888:
        for (r = 0; r < out_h; r++)
            for (x = 0; x < out_w; x++) {
                size_t s = (size_t)(bottom_up ? out_h - 1 - r : r) * stride + x,
                       d = (size_t)r * out_w + x;
                yuv888_to_rgb(words[s * 4], (int8_t)words[s * 4 + 1],
                              (int8_t)words[s * 4 + 2], &rgb[d * 3]);
            }
        break;
    case COL_PAL8:
        /* Acorn type 4 emits a word per pixel (index in the low byte); the
         * MovieFS Dec8 variant emits packed bytes (1 per pixel). */
        for (r = 0; r < out_h; r++)
            for (x = 0; x < out_w; x++) {
                size_t s = (size_t)(bottom_up ? out_h - 1 - r : r) * stride + x,
                       d = (size_t)r * out_w + x;
                unsigned idx = packed8 ? words[s] : words[s * 4];
                if (palette != NULL) {
                    rgb[d * 3 + 0] = palette[idx * 3 + 0];
                    rgb[d * 3 + 1] = palette[idx * 3 + 1];
                    rgb[d * 3 + 2] = palette[idx * 3 + 2];
                } else {
                    rgb[d * 3 + 0] = rgb[d * 3 + 1] = rgb[d * 3 + 2] =
                        (uint8_t)idx;
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
    AUDIO_ADPCM,       /* 4-bit IMA ADPCM: SoundA4 / "2 adpcm" */
    AUDIO_IOTA,        /* Iota TCA soundtrack (SOUN WAV1/WAV2), decoded to mono */
    AUDIO_ESCAPE_ADPCM /* Eidos Escape (sound format 101): WINSTR 4-bit ADPCM */
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
    case AUDIO_IOTA: return "Iota TCA sound (mono)";
    case AUDIO_ESCAPE_ADPCM: return "Eidos Escape 4-bit ADPCM";
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
    if (movie->video_codec == 500)
        return AUDIO_IOTA; /* type-500 Iota sound (codec 500): SOUN WAV1/WAV2 */
    if (movie->sound_codec == REPLAY_AE7_SOUND_NAMED)
        return ci_contains(movie->sound_format_label, "adpcm")
            ? AUDIO_ADPCM : AUDIO_UNKNOWN; /* GSM/G72x/MPEG: no decoder */
    /* Sound format 101 ("standard" / "linear unsigned"): the Eidos Escape movies.
     * The stock ARMovie player only accepts formats 1 and 2, so this is a
     * non-standard marker decoded by WINSTR's built-in sound routine: 4-bit
     * precision selects its (non-IMA) ADPCM, otherwise it is linear PCM. Despite
     * the "linear unsigned" label the 4-bit case is ADPCM. See
     * docs/spec/armovie-sound.md. */
    if (movie->sound_codec == 101) {
        if (movie->sound_precision == 4)
            return AUDIO_ESCAPE_ADPCM;
        if (movie->sound_precision == 8)
            return AUDIO_UNSIGNED_8; /* linear PCM path; unconfirmed (no sample) */
        if (movie->sound_precision == 16)
            return AUDIO_SIGNED_16;
        return AUDIO_UNKNOWN;
    }
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

/* Decode one chunk's sound region into interleaved signed-16 LE PCM, appended
 * to `pcm`. Reversed stereo (channels label contains "REVER") is normalised to
 * canonical left,right within the bytes this call appends, so the result is
 * identical whether audio is decoded a chunk at a time (NUT interleave) or all
 * at once (WAV). */
static void decode_chunk_audio(const ReplayAe7Movie *movie, const uint8_t *data,
                               size_t data_len, AudioFormat format, size_t c,
                               ReplayBuffer *pcm)
{
    const ReplayAe7Chunk *chunk = &movie->chunks[c];
    size_t soff = (size_t)(chunk->file_offset + chunk->video_bytes);
    size_t sbytes = (size_t)chunk->sound_bytes;
    size_t start = pcm->size;
    const uint8_t *s;
    size_t i;

    /* The Iota (type 500) soundtrack lives in the film's SOUN chunk, not the
     * per-chunk sound region; decode the whole track once, with chunk 0. */
    if (format == AUDIO_IOTA) {
        size_t off, cnt = 0;
        int16_t *p;
        if (c != 0)
            return;
        off = (size_t)movie->chunks[0].file_offset;
        if (off >= data_len)
            return;
        p = replay_tca_decode_audio(data + off, data_len - off, &cnt, NULL, 0);
        if (p != NULL) {
            for (i = 0; i < cnt; i++)
                append_s16le(pcm, p[i]);
            free(p);
        }
        return;
    }

    /* Eidos Escape (sound format 101): WINSTR's 4-bit ADPCM keeps one running
     * state for the whole movie (no per-chunk reset or header), so decode every
     * chunk's sound region in order on chunk 0. Each byte holds two codes, high
     * nibble first; for stereo the high nibble is left, the low nibble right. */
    if (format == AUDIO_ESCAPE_ADPCM) {
        ReplayEscapeAdpcmState s0, s1;
        int stereo = (movie->sound_channels == 2);
        size_t cc;
        if (c != 0)
            return;
        replay_escape_adpcm_init(&s0);
        replay_escape_adpcm_init(&s1);
        for (cc = 0; cc < movie->chunk_count; cc++) {
            const ReplayAe7Chunk *ch = &movie->chunks[cc];
            size_t so = (size_t)(ch->file_offset + ch->video_bytes);
            size_t sb = (size_t)ch->sound_bytes;
            size_t k;
            if (sb == 0)
                continue;
            if (so > data_len || sb > data_len - so)
                die("chunk %zu sound region is out of range", cc);
            for (k = 0; k < sb; k++) {
                uint8_t b = data[so + k];
                ReplayEscapeAdpcmState *lo = stereo ? &s1 : &s0;
                append_s16le(pcm, (int16_t)replay_escape_adpcm_decode_nibble(
                                      &s0, (unsigned)(b >> 4)));
                append_s16le(pcm, (int16_t)replay_escape_adpcm_decode_nibble(
                                      lo, (unsigned)b));
            }
        }
        return;
    }

    if (sbytes == 0)
        return;
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

    /* Reversed stereo: swap each L/R pair (within this chunk's appended bytes). */
    if (movie->sound_channels == 2
        && ci_contains(movie->sound_channels_label, "rever")) {
        size_t k;
        for (k = start; k + 3 < pcm->size; k += 4) {
            uint8_t *p = pcm->data + k;
            uint8_t t0 = p[0], t1 = p[1];
            p[0] = p[2]; p[1] = p[3]; p[2] = t0; p[3] = t1;
        }
    }
}

/* Decode every chunk's sound region into interleaved signed-16 LE PCM. */
static void decode_audio(const ReplayAe7Movie *movie, const uint8_t *data,
                         size_t data_len, AudioFormat format,
                         ReplayBuffer *pcm)
{
    size_t c;
    for (c = 0; c < movie->chunk_count; c++)
        decode_chunk_audio(movie, data, data_len, format, c, pcm);
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
/* Output sink: raw RGB24 (with a sidecar WAV) or a muxed NUT stream.   */
/* ------------------------------------------------------------------ */

typedef enum { SINK_RAW, SINK_NUT } SinkMode;

typedef struct {
    SinkMode mode;
    FILE *raw_out;                /* SINK_RAW: RGB24 frames go straight here */
    ReplayNutMuxer *nut;          /* SINK_NUT */
    size_t video_stream;          /* NUT stream indices */
    size_t audio_stream;
    int has_audio;                /* NUT: interleave the sound track */
    unsigned channels;
    AudioFormat audio_format;
    const ReplayAe7Movie *movie;
    const uint8_t *movie_data;
    size_t movie_len;
    uint64_t video_pts;           /* next frame index (video time base) */
    uint64_t audio_pts;           /* next per-channel sample (audio time base) */
} MuxSink;

static void sink_video_frame(MuxSink *sink, const uint8_t *rgb, size_t bytes)
{
    if (sink->mode == SINK_RAW) {
        if (fwrite(rgb, 1, bytes, sink->raw_out) != bytes)
            die("write error");
    } else {
        if (replay_nut_write_frame(sink->nut, sink->video_stream,
                                   (int64_t)sink->video_pts, 1, rgb, bytes)
            != REPLAY_OK)
            die("nut: video frame write failed");
        sink->video_pts++;
    }
}

/* NUT interleave: emit chunk c's decoded sound (if any) as one audio frame. */
static void sink_chunk_audio(MuxSink *sink, size_t c)
{
    ReplayBuffer pcm;
    size_t frame_bytes;
    if (sink->mode != SINK_NUT || !sink->has_audio)
        return;
    replay_buffer_init(&pcm);
    decode_chunk_audio(sink->movie, sink->movie_data, sink->movie_len,
                       sink->audio_format, c, &pcm);
    frame_bytes = 2u * (sink->channels != 0 ? sink->channels : 1u);
    if (pcm.size != 0) {
        if (replay_nut_write_frame(sink->nut, sink->audio_stream,
                                   (int64_t)sink->audio_pts, 1, pcm.data,
                                   pcm.size) != REPLAY_OK)
            die("nut: audio frame write failed");
        sink->audio_pts += pcm.size / frame_bytes;
    }
    replay_buffer_free(&pcm);
}

/* Pass-through: don't decode -- mux each chunk's (de-wrapped) codec frames
 * straight into the NUT video stream (whose fourcc the caller set to the codec
 * tag) and let ffmpeg decode them. The interleaved sound is emitted per chunk,
 * exactly as in the decode path. Returns the number of frames written. The
 * wrapper iterator skips any zero-word inter-frame padding. */
static unsigned long passthrough_video(const ReplayAe7Movie *movie,
                                       const uint8_t *movie_data,
                                       size_t movie_len, const CodecInfo *info,
                                       MuxSink *sink)
{
    unsigned long total = 0;
    size_t c;

    for (c = 0; c < movie->chunk_count; c++) {
        const ReplayAe7Chunk *chunk = &movie->chunks[c];
        size_t voff = (size_t)chunk->file_offset;
        size_t vbytes = (size_t)chunk->video_bytes;
        ReplayFrameWrapIter it;
        const uint8_t *frame;
        size_t frame_len;

        sink_chunk_audio(sink, c);

        if (vbytes == 0)
            continue;
        if (voff > movie_len || vbytes > movie_len - voff)
            die("chunk %zu video region is out of range", c);

        replay_frame_wrap_iter_init(&it, movie_data + voff, vbytes,
                                    info->passthrough_wrap);
        while (replay_frame_wrap_iter_next(&it, &frame, &frame_len, NULL)) {
            sink_video_frame(sink, frame, frame_len);
            total++;
        }
    }
    return total;
}

/* Decode all video frames to RGB24 and push them (and, in NUT mode, the
 * interleaved sound) through `sink`. Returns the number of frames written. */
static unsigned long transcode_video(const ReplayAe7Movie *movie,
                                     const uint8_t *movie_data,
                                     size_t movie_len, const CodecInfo *info,
                                     const char *module_path,
                                     const char *modules_dir,
                                     MuxSink *sink)
{
    char err[256];
    char module_buf[1024];
    size_t pixel_count = (size_t)movie->width * movie->height;
    int use_module = !info->direct_tca && !info->direct_esc100
                  && !info->direct_esc122 && !info->direct_esc124
                  && !info->direct_esc130
                  && !(info->direct_type23 && module_path == NULL);
    unsigned block_x = 1, block_y = 1;
    unsigned rounded_w = movie->width, rounded_h = movie->height;
    size_t rounded_count;
    MbPixel *pixels;
    uint8_t *rgb;
    unsigned long total = 0;
    VideoColour colour = info->colour;
    const uint8_t *palette = NULL;

    /* Resolve the decompressor and its block size up-front so the decoder runs
     * at block-rounded dimensions (with a buffer to match); the declared size
     * is cropped back on output. The native type-23 path needs no rounding. */
    if (use_module) {
        if (module_path == NULL) {
            if (modules_dir == NULL)
                die("codec %u needs a decompressor: pass --module or "
                    "--modules-dir", movie->video_codec);
            snprintf(module_buf, sizeof module_buf, "%s/%s",
                     modules_dir, info->module_subpath);
            module_path = module_buf;
        }
        if (!info->exact_size)
            info_block_for(movie->video_codec, module_path, NULL,
                           &block_x, &block_y);
        rounded_w = block_round_up(movie->width, block_x);
        rounded_h = block_round_up(movie->height, block_y);
    }
    rounded_count = (size_t)rounded_w * rounded_h;

    pixels = malloc((rounded_count ? rounded_count : 1) * sizeof(MbPixel));
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
    if (rounded_w != movie->width || rounded_h != movie->height)
        fprintf(stderr, "%s: decoding at %ux%u (block %ux%u), cropping to "
                "%ux%u\n", prog, rounded_w, rounded_h, block_x, block_y,
                movie->width, movie->height);

    if (info->direct_tca) {
        /* Iota Complete Animator (TCA/ACEF): the whole film is embedded in the
         * Replay container from the first chunk's offset; decode it natively to
         * 8bpp + its own palette. Iota sound (codec 500) is not decoded. */
        size_t off = movie->chunk_count ? (size_t)movie->chunks[0].file_offset : 0;
        ReplayTca *tca;
        uint8_t *idx;
        unsigned tw, th, bpp;
        if (movie->chunk_count == 0 || off >= movie_len)
            die("type 500: no film data");
        tca = replay_tca_open(movie_data + off, movie_len - off, err, sizeof err);
        if (tca == NULL)
            die("type 500: %s", err);
        tw = replay_tca_width(tca);
        th = replay_tca_height(tca);
        bpp = replay_tca_bpp(tca);
        if (tw != movie->width || th != movie->height)
            die("type 500: film %ux%u != header %ux%u", tw, th,
                movie->width, movie->height);
        fprintf(stderr, "%s: codec 500: %u TCA frames (%ubpp)\n", prog,
                replay_tca_frame_count(tca), bpp);
        /* 8bpp films emit one index byte per pixel; 16bpp films emit packed
         * RGB555 (two bytes per pixel). */
        idx = malloc((pixel_count ? pixel_count : 1) * (bpp == 16 ? 2 : 1));
        if (idx == NULL)
            die("out of memory");
        /* Emit the whole Iota soundtrack up front (it is not chunk-iterated). */
        sink_chunk_audio(sink, 0);
        for (;;) {
            int r = replay_tca_next_frame(tca, idx, err, sizeof err);
            if (r < 0)
                die("type 500: %s", err);
            if (r == 0)
                break;
            if (bpp == 16) {
                size_t i;
                for (i = 0; i < pixel_count; i++) {
                    unsigned v = idx[i * 2] | ((unsigned)idx[i * 2 + 1] << 8);
                    unsigned rr = v & 0x1F, g = (v >> 5) & 0x1F,
                             b = (v >> 10) & 0x1F;
                    rgb[i * 3 + 0] = (uint8_t)((rr << 3) | (rr >> 2));
                    rgb[i * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
                    rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
                }
            } else {
                convert_frame(COL_PAL8, idx, pixels, movie->width, movie->width,
                              movie->height, replay_tca_palette(tca), 1, 0, rgb);
            }
            sink_video_frame(sink, rgb, pixel_count * 3);
            total++;
        }
        free(idx);
        replay_tca_close(tca);
    } else if (info->direct_esc122) {
        /* Eidos Escape 122 (PAL8): each chunk is a whole 122 frame
         * ([codec_id 0x116][vsize][palette][bitstream]). Decode natively; the
         * frame's 8-bit indices and the per-chunk palette go through COL_PAL8. */
        ReplayEsc122 *esc = replay_esc122_open(movie->width, movie->height);
        size_t c;
        if (esc == NULL)
            die("type 122: out of memory");
        fprintf(stderr, "%s: codec 122 (Escape 122, PAL8), %ux%u\n", prog,
                movie->width, movie->height);
        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            sink_chunk_audio(sink, c);
            if (vbytes == 0)
                continue;
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);
            /* a malformed chunk (< 0) leaves the previous frame in place */
            (void)replay_esc122_decode(esc, movie_data + voff, vbytes);
            convert_frame(COL_PAL8, replay_esc122_frame(esc), pixels,
                          movie->width, movie->width, movie->height,
                          replay_esc122_palette(esc), 1, 0, rgb);
            sink_video_frame(sink, rgb, pixel_count * 3);
            total++;
        }
        replay_esc122_close(esc);
    } else if (info->direct_esc100) {
        /* Eidos Escape 100/102: a 5-bit-YUV 2x2-block VQ codec. Each video chunk
         * concatenates several frames, each `[u32 id][bitstream]` (id 0x100/0x102);
         * the decoder returns each frame's word-aligned length so we can walk them.
         * Output is YUV555 words, converted to RGB24 by the shared COL_YUV555 path. */
        ReplayEsc100 *esc = replay_esc100_open(movie->width, movie->height);
        uint8_t *ywords = malloc((pixel_count ? pixel_count : 1) * 4);
        size_t c;
        if (esc == NULL || ywords == NULL)
            die("type 100/102: bad dimensions or out of memory");
        fprintf(stderr, "%s: codec %u (Escape %u, YUV555), %ux%u\n", prog,
                movie->video_codec, movie->video_codec, movie->width, movie->height);
        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            size_t pos;
            sink_chunk_audio(sink, c);
            if (vbytes == 0)
                continue;
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);
            for (pos = 0; pos + 4 <= vbytes; ) {
                size_t used = replay_esc100_decode(esc, movie_data + voff + pos,
                                                   vbytes - pos);
                const uint16_t *fr;
                size_t i;
                if (used == 0)
                    break;                 /* malformed / end of this chunk */
                fr = replay_esc100_frame(esc);
                for (i = 0; i < pixel_count; i++) {
                    ywords[i * 4 + 0] = (uint8_t)fr[i];
                    ywords[i * 4 + 1] = (uint8_t)(fr[i] >> 8);
                    ywords[i * 4 + 2] = 0;
                    ywords[i * 4 + 3] = 0;
                }
                convert_frame(COL_YUV555, ywords, pixels, movie->width,
                              movie->width, movie->height, NULL, 0, 0, rgb);
                sink_video_frame(sink, rgb, pixel_count * 3);
                total++;
                pos += used;
            }
        }
        free(ywords);
        replay_esc100_close(esc);
    } else if (info->direct_esc124) {
        /* Eidos Escape 124 (RGB555): each video chunk concatenates
         * `frames_per_chunk` frames, each `[u32 flags][u32 size][bitstream]`
         * where `size` is the frame's total length (8-byte header included).
         * Decode natively; inter-frame deltas reference the immediately preceding
         * frame, so walk every frame in order. */
        ReplayEsc124 *esc = replay_esc124_open(movie->width, movie->height);
        size_t c;
        if (esc == NULL)
            die("type 124: bad dimensions (must be multiples of 8) or no memory");
        fprintf(stderr, "%s: codec 124 (Escape 124, RGB555), %ux%u\n", prog,
                movie->width, movie->height);
        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            size_t pos;
            sink_chunk_audio(sink, c);
            if (vbytes == 0)
                continue;
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);
            for (pos = 0; pos + 8 <= vbytes; ) {
                const uint8_t *fp = movie_data + voff + pos;
                size_t fsize = fp[4] | ((size_t)fp[5] << 8)
                             | ((size_t)fp[6] << 16) | ((size_t)fp[7] << 24);
                const uint16_t *fr;
                size_t i;
                if (fsize < 8 || pos + fsize > vbytes)
                    break;             /* malformed / end of this chunk's frames */
                (void)replay_esc124_decode(esc, fp, fsize);
                fr = replay_esc124_frame(esc);
                for (i = 0; i < pixel_count; i++) {
                    unsigned v = fr[i];  /* RGB555, red high; bit 15 is alpha */
                    unsigned rr = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
                    rgb[i * 3 + 0] = (uint8_t)((rr << 3) | (rr >> 2));
                    rgb[i * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
                    rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
                }
                sink_video_frame(sink, rgb, pixel_count * 3);
                total++;
                pos += fsize;
            }
        }
        replay_esc124_close(esc);
    } else if (info->direct_esc130) {
        /* Eidos Escape 2.0 (130): a YCbCr 2x2-block codec, one frame per chunk
         * (16-byte header + LSB-first bitstream). Decode updates the persistent
         * block state; render it to RGB888. Chunks < 16 bytes are "no change" and
         * re-emit the current picture, keeping one output frame per chunk. */
        ReplayEsc130 *esc = replay_esc130_open(movie->width, movie->height);
        size_t c;
        if (esc == NULL)
            die("type 130: bad dimensions (must be even) or no memory");
        fprintf(stderr, "%s: codec 130 (Escape 2.0), %ux%u\n", prog,
                movie->width, movie->height);
        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            sink_chunk_audio(sink, c);
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);
            (void)replay_esc130_decode(esc, movie_data + voff, vbytes);
            replay_esc130_render(esc, rgb);
            sink_video_frame(sink, rgb, pixel_count * 3);
            total++;
        }
        replay_esc130_close(esc);
    } else if (!use_module) {
        /* Fixed-size packed frames: unpack directly, no ARM decoder. An
         * explicit --module forces the decompressor route instead. The native
         * path is not chunk-iterated, so in NUT mode the per-chunk sound is
         * emitted up front (still as per-chunk packets with ascending pts). */
        size_t frame_count = 0, fi, c;
        if (replay_type23_frame_count(movie, &frame_count) != REPLAY_OK)
            die("type 23: bad frame layout");
        for (c = 0; c < movie->chunk_count; c++)
            sink_chunk_audio(sink, c);
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
            sink_video_frame(sink, rgb, pixel_count * 3);
            total++;
        }
    } else {
        size_t module_len;
        uint8_t *module;
        ReplayCodecIf *cif;
        size_t frame_words;
        uint8_t *out_words;
        uint8_t *unwrap = NULL;     /* scratch for de-wrapped MovieFS payload */
        uint8_t chunk_palette[256 * 3]; /* CRAM8: per-chunk inline palette */
        size_t c;

        module = read_file(module_path, &module_len);
        cif = replay_codecif_open(module, module_len, rounded_w, rounded_h,
                                  info->arm_mode_32 ? REPLAY_ARM_MODE_32
                                                    : REPLAY_ARM_MODE_26,
                                  err, sizeof err);
        if (cif == NULL) die("%s", err);
        frame_words = replay_codecif_frame_words_len(cif);
        out_words = malloc(frame_words);
        if (out_words == NULL) die("out of memory");

        /* The de-wrap scratch must hold the largest chunk's video payload
         * (stripped output is always <= input). */
        if (info->moviefs_wrapper) {
            size_t max_vbytes = 0;
            for (c = 0; c < movie->chunk_count; c++)
                if (movie->chunks[c].video_bytes > max_vbytes)
                    max_vbytes = (size_t)movie->chunks[c].video_bytes;
            if (max_vbytes != 0 && (unwrap = malloc(max_vbytes)) == NULL)
                die("out of memory");
        }

        for (c = 0; c < movie->chunk_count; c++) {
            const ReplayAe7Chunk *chunk = &movie->chunks[c];
            size_t voff = (size_t)chunk->file_offset;
            size_t vbytes = (size_t)chunk->video_bytes;
            const uint8_t *vdata;
            size_t offset = 0;
            /* Frames to decode this chunk: frames_per_chunk normally, but the
             * MovieFS wrapper tells us the exact count so we never run the
             * decoder past the end (a runaway WSS frame burns the whole budget). */
            unsigned frames_this_chunk = movie->frames_per_chunk;
            unsigned f;

            /* Interleave this chunk's sound ahead of its video frames. */
            sink_chunk_audio(sink, c);

            if (vbytes == 0)
                continue;
            if (voff > movie_len || vbytes > movie_len - voff)
                die("chunk %zu video region is out of range", c);

            vdata = movie_data + voff;
            /* CRAM8 (moviefs_palette): each chunk begins with a 0xffffffff marker
             * and a 256-entry RGB555 palette (32-bit words, red low), then the
             * wrapper chain. Extract the palette and skip the 1028-byte prefix. */
            if (info->moviefs_palette) {
                size_t k;
                if (vbytes < 1028
                    || vdata[0] != 0xff || vdata[1] != 0xff
                    || vdata[2] != 0xff || vdata[3] != 0xff)
                    die("chunk %zu: expected CRAM8 palette marker", c);
                for (k = 0; k < 256; k++) {
                    const uint8_t *e = vdata + 4 + k * 4;
                    unsigned v = e[0] | ((unsigned)e[1] << 8);
                    unsigned rr = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
                    chunk_palette[k * 3 + 0] = (uint8_t)((rr << 3) | (rr >> 2));
                    chunk_palette[k * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
                    chunk_palette[k * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
                }
                palette = chunk_palette;
                vdata += 1028;
                vbytes -= 1028;
            }
            if (info->moviefs_wrapper) {
                size_t unwrap_len = 0;
                frames_this_chunk = (unsigned)replay_moviefs_unwrap_chunk(
                    vdata, vbytes, unwrap, &unwrap_len);
                if (frames_this_chunk == 0)
                    die("chunk %zu: no MovieFS frames found (bad wrapper?)", c);
                vdata = unwrap;
                vbytes = unwrap_len;
            }

            if (replay_codecif_load_payload(cif, vdata, vbytes,
                                            err, sizeof err) != 0)
                die("chunk %zu: %s", c, err);

            for (f = 0; f < frames_this_chunk; f++) {
                size_t consumed = 0;
                if (replay_codecif_decode(cif, &offset, out_words, &consumed,
                                          err, sizeof err) != 0) {
                    if (c + 1 == movie->chunk_count)
                        break; /* final chunk may be short / padded */
                    die("chunk %zu frame %u: %s", c, f, err);
                }
                convert_frame(colour, out_words, pixels, rounded_w,
                              movie->width, movie->height, palette,
                              info->packed8, info->bottom_up, rgb);
                sink_video_frame(sink, rgb, pixel_count * 3);
                total++;
            }
        }
        free(unwrap);
        free(out_words);
        replay_codecif_close(cif);
        free(module);
    }

    free(rgb);
    free(pixels);
    return total;
}

static VideoColour video_colour_from_name(const char *name)
{
    if (!strcmp(name, "6y5uv")) return COL_6Y5UV;
    if (!strcmp(name, "6y6uv")) return COL_6Y6UV;
    if (!strcmp(name, "yuv555")) return COL_YUV555;
    if (!strcmp(name, "rgb555")) return COL_RGB555;
    if (!strcmp(name, "yuv888")) return COL_YUV888;
    if (!strcmp(name, "rgb888")) return COL_RGB888;
    if (!strcmp(name, "palette") || !strcmp(name, "pal8")) return COL_PAL8;
    die("unknown --video-colour '%s' "
        "(6y5uv|6y6uv|yuv555|rgb555|yuv888|rgb888|palette)", name);
    return COL_6Y5UV;
}

/* NUT codec tags, in the on-disk order ffmpeg writes them (little-endian). */
static const uint8_t NUT_FOURCC_RGB24[4] = { 'R', 'G', 'B', 24 };
static const uint8_t NUT_FOURCC_PCM_S16LE[4] = { 'P', 'S', 'D', 16 };

static unsigned gcd_u(unsigned a, unsigned b)
{
    while (b != 0) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    return a != 0 ? a : 1u;
}

/* Express a frame rate as a NUT time base: time(frame i) = i * num/den. */
static void rational_from_fps(double fps, unsigned *num, unsigned *den)
{
    unsigned n, d, g;
    if (!(fps > 0.0)) {
        *num = 1u;
        *den = 25u;
        return;
    }
    d = (unsigned)(fps * 1000.0 + 0.5);  /* den ~ fps*1000, num = 1000 => 1/fps */
    if (d == 0)
        d = 1u;
    n = 1000u;
    g = gcd_u(n, d);
    *num = n / g;
    *den = d / g;
}

/* MovieFS codecs carry the true frame size in the per-frame wrapper; the AE7
 * header rounds the width up (e.g. 156 -> 160). Read chunk 0's first wrapper to
 * recover it. CRAM8 (moviefs_palette) has a 0xffffffff marker + 256-word palette
 * before the wrapper. Returns 0 and sets the w and h out-params on success. */
static int moviefs_true_size(const uint8_t *movie_data, size_t movie_len,
                             const ReplayAe7Movie *movie, const CodecInfo *info,
                             unsigned *w, unsigned *h)
{
    size_t off, n;
    const uint8_t *p;
    if (movie->chunk_count == 0)
        return -1;
    off = (size_t)movie->chunks[0].file_offset;
    n = (size_t)movie->chunks[0].video_bytes;
    if (off > movie_len || n > movie_len - off)
        return -1;
    p = movie_data + off;
    if (info->moviefs_palette) {           /* skip 0xffffffff + 256-word palette */
        if (n < 1028)
            return -1;
        p += 1028;
        n -= 1028;
    }
    if (n < 16)
        return -1;
    *w = p[8] | ((unsigned)p[9] << 8) | ((unsigned)p[10] << 16)
       | ((unsigned)p[11] << 24);
    *h = p[12] | ((unsigned)p[13] << 8) | ((unsigned)p[14] << 16)
       | ((unsigned)p[15] << 24);
    if (*w == 0 || *h == 0 || *w > 4096 || *h > 4096)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL, *module_path = NULL, *modules_dir = NULL;
    const char *output_path = NULL, *audio_output = NULL;
    const char *audio_format_name = NULL, *video_colour_name = NULL;
    const char *output_format = "nut";
    int skip_unsupported = 0;
    int nut_mode;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (++i < argc ? argv[i] : (die("%s needs a value", a), ""))
        if (strcmp(a, "--input") == 0) input_path = NEXT();
        else if (strcmp(a, "--module") == 0) module_path = NEXT();
        else if (strcmp(a, "--modules-dir") == 0) modules_dir = NEXT();
        else if (strcmp(a, "--output") == 0) output_path = NEXT();
        else if (strcmp(a, "--output-format") == 0) output_format = NEXT();
        else if (strcmp(a, "--audio-output") == 0) audio_output = NEXT();
        else if (strcmp(a, "--audio-format") == 0) audio_format_name = NEXT();
        else if (strcmp(a, "--video-colour") == 0) video_colour_name = NEXT();
        else if (strcmp(a, "--skip-unsupported") == 0) skip_unsupported = 1;
        else die("unknown argument '%s'", a);
#undef NEXT
    }
    if (input_path == NULL) die("--input MOVIE is required");
    if (strcmp(output_format, "raw") != 0 && strcmp(output_format, "nut") != 0)
        die("unknown --output-format '%s' (raw|nut)", output_format);
    nut_mode = (strcmp(output_format, "nut") == 0);

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
    char generic_name[80];
    int have_video = (codec_info(movie.video_codec, &info) == 0);
    /* Video format 0 means "no video track" (a sound-only movie), not an
     * unknown codec: there is no Decomp0 decompressor, so it must skip both the
     * external-decompressor fallback below and the unsupported-codec error. We
     * key on the codec number alone — some real sound-only movies (!ARPlayer's
     * DUMMY, the CineClips collection) wrongly carry non-zero dimensions in the
     * header, so the geometry can't be trusted to identify them. */
    int sound_only = (movie.video_codec == 0);
    if (sound_only) {
        fprintf(stderr, "%s: video format 0: sound-only movie, no video track\n",
                prog);
    } else if (!have_video && (module_path != NULL || modules_dir != NULL)) {
        /* Unknown codec, but we have a decompressor: drive it generically.
         * Colour-model priority: --video-colour, then a real Info colour line
         * (e.g. "YUV 8,8,8"), then the movie header's pixel label ("16 bits per
         * pixel YUV" etc.). The last is how Escape — whose Info carries no
         * colour line — correctly resolves to YUV555, like the Acorn Player. */
        VideoColour col = COL_6Y5UV;
        int multi = 0;
        memset(&info, 0, sizeof info);
        snprintf(generic_sub, sizeof generic_sub,
                 "Decomp%u/Decompress,ffd", movie.video_codec);
        info.module_subpath = generic_sub;
        /* Label the codec from its Info file's name line (e.g. "Escape") when
         * available, falling back to a bare "external". */
        if (info_name_for(movie.video_codec, module_path, modules_dir,
                          generic_name, sizeof generic_name) != 0)
            snprintf(generic_name, sizeof generic_name, "external");
        info.name = generic_name;
        if (video_colour_name != NULL) {
            info.colour = video_colour_from_name(video_colour_name);
        } else if (info_colour_for(movie.video_codec, module_path, modules_dir,
                                   &col, &multi) == 0 && !multi) {
            info.colour = col; /* an explicit colour line in the Info file */
        } else {
            info.colour = movie.pixel_depth >= 24 ? COL_RGB888 : COL_RGB555;
            info.header_colour = 1; /* refine from the header pixel label */
        }
        have_video = 1;
        fprintf(stderr, "%s: codec %u (%s): external decompressor, %s colour\n",
                prog, movie.video_codec, info.name,
                video_colour_name ? "--video-colour"
                : !info.header_colour ? "Info colour line" : "header");
    }
    /* --video-colour overrides the colour model for any codec. */
    if (have_video && video_colour_name != NULL) {
        info.colour = video_colour_from_name(video_colour_name);
        info.header_colour = 0;
    }
    if (!have_video && !sound_only) {
        if (skip_unsupported)
            fprintf(stderr, "%s: skipping video: codec %u is not supported\n",
                    prog, movie.video_codec);
        else
            die("video codec %u is not supported "
                "(supply --module/--modules-dir for an external decompressor, "
                "or --skip-unsupported for audio-only output)",
                movie.video_codec);
    }

    /* MovieFS codecs carry the true frame size in the per-frame wrapper; the AE7
     * header rounds the width up. Override the geometry so the whole pipeline
     * (decoder, conversion, NUT stream) uses the true size. */
    if (have_video && info.moviefs_wrapper) {
        unsigned tw = 0, th = 0;
        if (moviefs_true_size(movie_data, movie_len, &movie, &info, &tw, &th)
                == 0 && (tw != movie.width || th != movie.height)) {
            fprintf(stderr, "%s: MovieFS true size %ux%u (header says %ux%u)\n",
                    prog, tw, th, movie.width, movie.height);
            movie.width = tw;
            movie.height = th;
        }
    }

    /* In NUT mode the sound track is always muxed (when present and decodable),
     * so a sidecar --audio-output is meaningless. In raw mode audio is written
     * only when --audio-output names a WAV file. */
    if (nut_mode && audio_output != NULL)
        fprintf(stderr, "%s: --audio-output ignored in nut mode (sound is "
                        "muxed into the stream)\n", prog);

    AudioFormat aud = AUDIO_NONE;
    int do_audio = 0;
    int want_audio = nut_mode || (audio_output != NULL);
    if (want_audio) {
        aud = audio_format_name != NULL
            ? audio_format_from_name(audio_format_name)
            : choose_audio_format(&movie);
        if (aud == AUDIO_NONE) {
            fprintf(stderr, "%s: movie has no sound track; no audio %s\n",
                    prog, nut_mode ? "muxed" : "written");
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
    if (!have_video && !do_audio) {
        /* A sound-only movie whose only sound track we cannot decode (or which
         * carries none — e.g. the DUMMY placeholder) has nothing to transcode,
         * but that is not a tool failure: report it and exit cleanly. */
        if (!sound_only)
            die("nothing to output");
        fprintf(stderr, "%s: sound-only movie has no decodable sound track; "
                        "nothing to transcode\n", prog);
        replay_ae7_movie_destroy(&movie);
        free(movie_data);
        return 0;
    }

    /* Pass-through codecs are remuxed into NUT for ffmpeg to decode; there is no
     * decoded RGB24 to write, so raw mode cannot represent them. */
    int passthrough = have_video && info.passthrough_fourcc != NULL;
    if (passthrough && !nut_mode)
        die("video codec %u (%s) is decoded by ffmpeg, not this tool; "
            "use --output-format nut", movie.video_codec, info.name);

    /* ---- set up the output sink ---- */
    FILE *out = stdout;
    int need_out = nut_mode || have_video;
    if (need_out && output_path != NULL) {
        out = fopen(output_path, "wb");
        if (out == NULL)
            die("cannot create %s", output_path);
    }

    MuxSink sink;
    memset(&sink, 0, sizeof sink);
    sink.movie = &movie;
    sink.movie_data = movie_data;
    sink.movie_len = movie_len;
    sink.audio_format = aud;
    /* Iota TCA sound is decoded to mono regardless of the header's channel
     * count (which can be a stray value on type-500 movies). */
    sink.channels = aud == AUDIO_IOTA ? 1u
                  : movie.sound_channels != 0 ? movie.sound_channels : 1u;

    ReplayNutMuxer *nut = NULL;
    if (nut_mode) {
        ReplayNutStream streams[2];
        size_t nstreams = 0;
        memset(streams, 0, sizeof streams);
        sink.mode = SINK_NUT;
        if (have_video) {
            unsigned tbn, tbd;
            rational_from_fps(movie.frames_per_second, &tbn, &tbd);
            streams[nstreams].cls = REPLAY_NUT_VIDEO;
            if (passthrough)
                memcpy(streams[nstreams].fourcc, info.passthrough_fourcc, 4);
            else
                memcpy(streams[nstreams].fourcc, NUT_FOURCC_RGB24, 4);
            streams[nstreams].tb_num = tbn;
            streams[nstreams].tb_den = tbd;
            streams[nstreams].width = movie.width;
            streams[nstreams].height = movie.height;
            sink.video_stream = nstreams++;
        }
        if (do_audio) {
            streams[nstreams].cls = REPLAY_NUT_AUDIO;
            memcpy(streams[nstreams].fourcc, NUT_FOURCC_PCM_S16LE, 4);
            streams[nstreams].tb_num = 1u;
            streams[nstreams].tb_den = movie.sound_rate != 0 ? movie.sound_rate : 1u;
            streams[nstreams].sample_rate = movie.sound_rate;
            streams[nstreams].channels = sink.channels;
            sink.audio_stream = nstreams++;
            sink.has_audio = 1;
        }
        nut = replay_nut_open(out, streams, nstreams, err, sizeof err);
        if (nut == NULL)
            die("%s", err);
        sink.nut = nut;
    } else {
        sink.mode = SINK_RAW;
        sink.raw_out = out;
        /* Raw mode: write the sidecar WAV up front so a streamed pipeline can
         * open it while the RGB24 video is still flowing on stdout. */
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
    }

    /* ---- video (and, in NUT mode, the interleaved sound) ---- */
    unsigned long total = 0;
    if (passthrough) {
        total = passthrough_video(&movie, movie_data, movie_len, &info, &sink);
    } else if (have_video) {
        total = transcode_video(&movie, movie_data, movie_len, &info,
                                module_path, modules_dir, &sink);
    } else if (nut_mode && do_audio) {
        /* Sound-only movie muxed as an audio-only NUT stream. */
        size_t c;
        for (c = 0; c < movie.chunk_count; c++)
            sink_chunk_audio(&sink, c);
    }

    if (nut != NULL && replay_nut_close(nut) != REPLAY_OK)
        die("nut: flush failed");
    if (out != stdout)
        fclose(out);

    /* ---- summary + ready-to-run ffmpeg command ---- */
    if (nut_mode) {
        if (have_video)
            fprintf(stderr,
                    "%s: wrote %lu frames + muxed sound (NUT)\n"
                    "%s: ... | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p "
                    "-c:a aac out.mp4\n",
                    prog, total, prog);
        else
            fprintf(stderr,
                    "%s: wrote audio-only NUT stream\n"
                    "%s: ... | ffmpeg -i - out.m4a\n", prog, prog);
    } else if (have_video && do_audio) {
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -i %s -c:v libx264 -pix_fmt yuv420p "
                "-c:a aac -shortest out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second, audio_output);
    } else if (have_video) {
        fprintf(stderr,
                "%s: wrote %lu frames\n"
                "%s: ffmpeg -f rawvideo -pixel_format rgb24 -video_size %ux%u "
                "-framerate %.6g -i - -c:v libx264 -pix_fmt yuv420p out.mp4\n",
                prog, total, prog, movie.width, movie.height,
                movie.frames_per_second);
    } else if (do_audio) {
        fprintf(stderr, "%s: audio-only; encode with: ffmpeg -i %s out.m4a\n",
                prog, audio_output);
    }

    replay_ae7_movie_destroy(&movie);
    free(movie_data);
    return 0;
}
