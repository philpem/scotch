#ifndef REPLAY_AE7_WRITE_H
#define REPLAY_AE7_WRITE_H

#include <stddef.h>
#include <stdint.h>

#include "replay/replay_buffer.h"
#include "replay/replay_status.h"

/*
 * Portable writer for the Acorn Replay ARMovie/AE7 container, reproducing the
 * layout produced by Acorn's `Join` tool: a 21-line text header, optional
 * sprite, optional per-chunk key-frame area, a text catalogue, then
 * sector-aligned chunks each holding [video][sound track 0][sound track 1]...
 *
 * Unlike Join, which concatenates one `Images` file per chunk, this writer is
 * given individual frame payloads and performs the chunking itself, mirroring
 * what CompLib's per-chunk save did in the original toolchain.
 *
 * See notes/replay-ae7-join-writer.md for the authoritative format.
 */

/* Default sector-alignment mask: (offset + mask) & ~mask, i.e. 2048-byte (CD
 * sector) chunk alignment. This is Join's default `-size` value. */
#define REPLAY_AE7_DEFAULT_ALIGN_MASK 2047U

/*
 * Sound compression identifiers used in header field 10. Per AE7doc the meaning
 * of this field is defined by the video codec, but two values are conventional:
 *   0 - no sound.
 *   1 - the built-in 8-bit VIDC "exponential" companding (VIDC1 mu-law).
 *   2 - the extensible "named decompressor" format. The line is written as
 *       "2 <name>" and the player loads that sound decompressor. ADPCM, G721,
 *       G723-1/24/40, GSM and MPEG-I/II are all format 2 with a name (e.g.
 *       "2 adpcm", "2 GSM"); see ARMovie_2003/Sound16/<name>.
 * Other numbers are accepted and written verbatim for codecs namespaced by an
 * unusual video format.
 */
#define REPLAY_AE7_SOUND_NONE 0U
#define REPLAY_AE7_SOUND_VIDC_LOG 1U /* 8-bit VIDC "exponential" (VIDC1 mu-law) */
#define REPLAY_AE7_SOUND_NAMED 2U    /* "2 <name>": adpcm, GSM, G723-1, ... */

typedef struct {
    unsigned codec;           /* sound format number (REPLAY_AE7_SOUND_*) */
    const char *codec_name;   /* decompressor name; required when codec == 2 */
    unsigned rate_hz;         /* samples per second */
    unsigned channels;        /* 1 or 2 */
    unsigned precision_bits;  /* source bits per sample (8, 16) */
    const char *label;        /* optional annotation, e.g. "8 bits per sample
                               * (exponential)"; NULL selects a default */
    /*
     * One contiguous, already-encoded track. The writer slices it per chunk by
     * the chunk's time span so audio stays aligned with video. If the track is
     * shorter than the video timeline the final chunks carry less (or no)
     * audio, exactly as Join tolerates a short track.
     *
     * Time slicing assumes a constant bytes-per-sample mapping (channels *
     * precision_bits/8), which is correct for format 1 (VIDC 8-bit) and
     * uncompressed linear PCM. Framed/variable-rate format-2 codecs (GSM, MPEG,
     * G72x) encode whole sample-frames per chunk and need their chunk boundaries
     * supplied explicitly; that path is not yet implemented.
     */
    const uint8_t *data;
    size_t size;
} ReplayAe7WriteTrack;

typedef struct {
    /* Text metadata. NULL is treated as an empty string. */
    const char *title;
    const char *copyright;
    const char *author;

    /* Video. */
    unsigned video_codec;     /* e.g. 19; 0 means a sound-only movie */
    unsigned width;
    unsigned height;
    unsigned pixel_depth;     /* bits per pixel, e.g. 16 */
    const char *pixel_label;  /* optional, e.g. "6Y5UV" */
    double frames_per_second;

    /* Frame payloads, in display order. */
    const uint8_t *const *frame_data;
    const size_t *frame_size;
    size_t frame_count;

    /*
     * Chunking. If frames_per_chunk is non-zero it is used directly (every
     * chunk holds that many frames, the last possibly fewer). Otherwise the
     * writer targets chunk_seconds of video and distributes frames with
     * floor((i+1)*F) - floor(i*F), F = fps * chunk_seconds, so fractional rates
     * alternate frame counts (e.g. 12,13,12,13 for 12.5fps and 1.0s).
     */
    unsigned frames_per_chunk;
    double chunk_seconds;     /* used only when frames_per_chunk == 0 */

    /* Sector-alignment mask; 0 selects REPLAY_AE7_DEFAULT_ALIGN_MASK. */
    unsigned align_mask;

    /* Optional sound tracks. */
    const ReplayAe7WriteTrack *tracks;
    size_t track_count;

    /*
     * Optional per-chunk key-frame blobs (the "Join keys" option). When
     * write_keys is non-zero, key_data must supply chunk_count blobs each of
     * key_size bytes; the writer emits the key area and sets the key-frame
     * offset. When zero, the movie is written with "-1 (no keys)".
     */
    int write_keys;
    const uint8_t *const *key_data;
    size_t key_size;

    /*
     * Optional "helpful sprite" (poster), stored as a complete standard RISC OS
     * spritefile. !ARPlayer reads it from sprite_offset+12 for sprite_size-12
     * bytes and crashes if it is absent (size 0 -> a -12 byte read), so a movie
     * intended for the GUI player must supply one. When NULL the movie is
     * written with sprite offset/size 0 (valid for the command-line player).
     */
    const uint8_t *sprite_data;
    size_t sprite_size;

    /* AREncode "Index": opaque pass-through for batch naming. Unused by the
     * single-file writer; recorded here for future segmented output. */
    unsigned index;
} ReplayAe7WriteOptions;

/*
 * Build a complete movie into `out` (which is cleared first). On failure a
 * human-readable message is written to `error` when provided. The function does
 * not transfer ownership of any input pointers.
 */
ReplayStatus replay_ae7_write(const ReplayAe7WriteOptions *options,
                              ReplayBuffer *out,
                              char *error, size_t error_size);

#endif
