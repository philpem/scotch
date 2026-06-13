#include "replay/replay_movie.h"

#include <stdio.h>
#include <string.h>

#include "replay/replay_sound.h"

static void put_u32(ReplayBuffer *b, uint32_t v)
{
    uint8_t bytes[4] = {
        (uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU),
        (uint8_t)((v >> 16) & 0xFFU), (uint8_t)((v >> 24) & 0xFFU)
    };

    (void)replay_buffer_append(b, bytes, sizeof(bytes));
}

/*
 * The mode field is a new-format sprite mode word selecting 16bpp (type 5,
 * 1:5:5:5) at 90x90 dpi, i.e. square pixels: (5<<27)|(90<<14)|(90<<1)|1. Old
 * numbered modes such as 28 are 8bpp and would render the 16bpp data as garbage
 * 256-colour pixels, and square pixels mean the poster is stored at the movie's
 * true dimensions (no 2:1 height doubling).
 */
#define POSTER_SPRITE_MODE ((5u << 27) | (90u << 14) | (90u << 1) | 1u)

ReplayStatus replay_build_poster(const uint8_t *pixels, unsigned width,
                                 unsigned height, ReplayBuffer *out)
{
    unsigned row_bytes = width * 2U;
    unsigned words_per_row = (row_bytes + 3U) / 4U;
    unsigned padded = words_per_row * 4U;
    unsigned image_size = padded * height;
    unsigned sprite_payload = 44U + image_size;
    unsigned last_bytes = ((row_bytes + 3U) % 4U) + 1U; /* bytes used, last word */
    static const char name[12] = "poster";
    unsigned y;

    replay_buffer_clear(out);
    put_u32(out, 1U);                          /* number of sprites */
    put_u32(out, 16U);                         /* offset to first sprite */
    put_u32(out, 12U + sprite_payload + 4U);   /* offset to first free word */
    put_u32(out, sprite_payload);              /* offset to next sprite */
    if (replay_buffer_append(out, name, sizeof(name)) != REPLAY_OK) {
        return REPLAY_OUT_OF_MEMORY;
    }
    put_u32(out, words_per_row - 1U);          /* width in words - 1 */
    put_u32(out, height - 1U);                 /* height in scan lines - 1 */
    put_u32(out, 0U);                          /* first bit used */
    put_u32(out, last_bytes * 8U - 1U);        /* last bit used */
    put_u32(out, 44U);                         /* offset to image */
    put_u32(out, 44U);                         /* offset to mask (= image) */
    put_u32(out, POSTER_SPRITE_MODE);          /* 16bpp, square pixels */
    for (y = 0U; y < height; ++y) {
        static const uint8_t zero[4] = { 0, 0, 0, 0 };

        if (replay_buffer_append(out, pixels + (size_t)y * row_bytes,
                                 row_bytes) != REPLAY_OK ||
            replay_buffer_append(out, zero, padded - row_bytes) != REPLAY_OK) {
            return REPLAY_OUT_OF_MEMORY;
        }
    }
    return REPLAY_OK;
}

ReplayStatus replay_build_pcm_track(const uint8_t *pcm, size_t pcm_size,
                                    const char *encode, unsigned rate_hz,
                                    unsigned channels, ReplayBuffer *encoded,
                                    ReplayAe7WriteTrack *track, char *error,
                                    size_t error_size)
{
    int is_sounda4 = strcmp(encode, "adpcm-sounda4") == 0;
    int is_adpcm = is_sounda4 || strcmp(encode, "adpcm") == 0 ||
                   strcmp(encode, "adpcm2") == 0;

    memset(track, 0, sizeof(*track));

    if (is_adpcm) {
        /* IMA ADPCM: hand the raw PCM to the writer, which encodes it per chunk.
         * "adpcm" is the named-decompressor form (format 2 "adpcm", the common
         * SoundDir module); "adpcm-sounda4" is the built-in format-1 SoundA4. */
        if (replay_buffer_append(encoded, pcm, pcm_size) != REPLAY_OK) {
            return REPLAY_OUT_OF_MEMORY;
        }
        track->encode_adpcm = 1;
        track->rate_hz = rate_hz;
        track->channels = channels;
        track->data = encoded->data;
        track->size = encoded->size;
        if (is_sounda4) {
            track->codec = REPLAY_AE7_SOUND_VIDC_LOG; /* format 1 */
            track->codec_name = NULL;
            track->precision_bits = 4U;
            track->label = "bit ADPCM";
        } else {
            track->codec = REPLAY_AE7_SOUND_NAMED;
            track->codec_name = "adpcm";
            track->precision_bits = 16U;
            track->label = NULL;
        }
        return REPLAY_OK;
    } else {
        /* Encode canonical signed-16 little-endian PCM into a Replay format-1
         * sub-format. The bits-per-sample label selects the player's decoder. */
        ReplaySoundFormat fmt;
        size_t count;
        size_t i;

        if (strcmp(encode, "signed-8") == 0) {
            fmt = REPLAY_SOUND_SIGNED_8;
        } else if (strcmp(encode, "signed-16") == 0) {
            fmt = REPLAY_SOUND_SIGNED_16;
        } else if (strcmp(encode, "vidc-e8") == 0 ||
                   strcmp(encode, "vidc-log") == 0) {
            fmt = REPLAY_SOUND_VIDC_E8;
        } else {
            if (error != NULL && error_size != 0U) {
                snprintf(error, error_size, "unknown sound encode: %s", encode);
            }
            return REPLAY_INVALID_ARGUMENT;
        }
        count = pcm_size / 2U; /* signed 16-bit little-endian samples */
        for (i = 0U; i < count; ++i) {
            int16_t sample =
                (int16_t)((uint16_t)pcm[i * 2U] |
                          ((uint16_t)pcm[i * 2U + 1U] << 8));

            if (replay_sound_encode(fmt, &sample, 1U, encoded) != REPLAY_OK) {
                if (error != NULL && error_size != 0U) {
                    snprintf(error, error_size, "audio encode failed");
                }
                return REPLAY_OUT_OF_MEMORY;
            }
        }
        track->codec = REPLAY_AE7_SOUND_VIDC_LOG; /* format 1 */
        track->codec_name = NULL;
        track->rate_hz = rate_hz;
        track->channels = channels;
        track->precision_bits = replay_sound_format_bits(fmt);
        track->label = replay_sound_format_label(fmt);
        track->data = encoded->data;
        track->size = encoded->size;
        return REPLAY_OK;
    }
}
