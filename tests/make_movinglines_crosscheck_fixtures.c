/*
 * Generate Moving Lines (type 1) cross-check fixtures. For each case this writes
 * the payload, the previous frame (when the case is an inter frame) and this
 * project's decoder output, all as native little-endian 16-bit pixels, plus a
 * manifest. tests/test_movinglines_compiled.sh then decodes each payload on the
 * genuine compiled MovingLine module (via tools/movinglines_unicorn.py) and
 * compares the module's output to the decoder output written here.
 *
 * Cases cover every command family: literals and repeats (the encoder's output),
 * same-position previous-frame copies (an inter frame), and hand-built temporal,
 * spatial and bit-packed long-literal streams the encoder does not itself emit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replay/codec_movinglines.h"
#include "replay/mb_repeat.h"
#include "replay/replay_buffer.h"

static int write_pixels(const char *dir, const char *stem, const char *ext,
                        const uint16_t *pixels, size_t count)
{
    char path[1024];
    FILE *file;
    size_t i;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s%s", dir, stem, ext) >=
        sizeof(path)) {
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return -1;
    }
    for (i = 0; i < count; ++i) {
        uint8_t bytes[2] = { (uint8_t)(pixels[i] & 0xFFU),
                             (uint8_t)((pixels[i] >> 8) & 0xFFU) };
        if (fwrite(bytes, 1, 2, file) != 2) {
            fclose(file);
            return -1;
        }
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int write_payload(const char *dir, const char *stem,
                         const ReplayBuffer *payload)
{
    char path[1024];
    FILE *file;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s.mln", dir, stem) >=
        sizeof(path)) {
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        return -1;
    }
    if (payload->size != 0 &&
        fwrite(payload->data, 1, payload->size, file) != payload->size) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

/* Decode `payload` with the reference codec, write payload + decoded output (and
   the previous frame, when present), and append a manifest line. */
static int emit(FILE *manifest, const char *dir, const char *stem,
                const ReplayBuffer *payload, const uint16_t *previous,
                unsigned width, unsigned height)
{
    uint16_t decoded[4096];
    size_t count = (size_t)width * height;

    if (count > sizeof(decoded) / sizeof(decoded[0])) {
        return -1;
    }
    if (codec_movinglines_decode_frame(payload->data, payload->size, previous,
                                       decoded, width, height,
                                       NULL) != REPLAY_OK) {
        fprintf(stderr, "%s: reference decode failed\n", stem);
        return -1;
    }
    if (write_payload(dir, stem, payload) != 0 ||
        write_pixels(dir, stem, ".expected16", decoded, count) != 0) {
        return -1;
    }
    if (previous != NULL &&
        write_pixels(dir, stem, ".prev16", previous, count) != 0) {
        return -1;
    }
    fprintf(manifest, "%s %u %u %d\n", stem, width, height,
            previous != NULL ? 1 : 0);
    return 0;
}

static void put16(ReplayBuffer *b, unsigned word)
{
    (void)replay_buffer_append_u8(b, (uint8_t)(word & 0xFFU));
    (void)replay_buffer_append_u8(b, (uint8_t)((word >> 8) & 0xFFU));
}

#define EOF_WORD (1U + (0x1CCU << 7U))

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char manifest_path[1024];
    FILE *manifest;
    ReplayBuffer payload;
    int result = EXIT_FAILURE;
    unsigned i;

    if ((size_t)snprintf(manifest_path, sizeof(manifest_path), "%s/manifest",
                         dir) >= sizeof(manifest_path)) {
        return EXIT_FAILURE;
    }
    manifest = fopen(manifest_path, "w");
    if (manifest == NULL) {
        perror(manifest_path);
        return EXIT_FAILURE;
    }
    replay_buffer_init(&payload);

    /* Case 1: a key frame from the encoder -- gradient literals plus a repeat. */
    {
        uint16_t key[32];

        for (i = 0; i < 32U; ++i) {
            key[i] = (uint16_t)((i * 37U) & 0x7FFFU);
        }
        for (i = 10U; i < 24U; ++i) {
            key[i] = 0x1234U;
        }
        if (codec_movinglines_encode_frame(key, NULL, 8U, 4U, &payload) !=
                REPLAY_OK ||
            emit(manifest, dir, "keyframe", &payload, NULL, 8U, 4U) != 0) {
            goto done;
        }

        /* Case 2: an inter frame against that key (same-position copies). */
        {
            uint16_t inter[32];

            memcpy(inter, key, sizeof(inter));
            inter[0] = 0x7FFFU;
            inter[5] = 0x0111U;
            inter[31] = 0x0001U;
            if (codec_movinglines_encode_frame(inter, key, 8U, 4U, &payload) !=
                    REPLAY_OK ||
                emit(manifest, dir, "interframe", &payload, key, 8U, 4U) != 0) {
                goto done;
            }
        }
    }

    /* Case 3: hand-built temporal copies (offsets +width then -width). */
    {
        uint16_t previous[16];

        for (i = 0; i < 16U; ++i) {
            previous[i] = (uint16_t)((i * 101U + 7U) & 0x7FFFU);
        }
        replay_buffer_clear(&payload);
        put16(&payload, 1U + (160U << 7U) + ((8U - 2U) << 1U));
        put16(&payload, 1U + (127U << 7U) + ((8U - 2U) << 1U));
        put16(&payload, EOF_WORD);
        if (emit(manifest, dir, "temporal", &payload, previous, 8U, 2U) != 0) {
            goto done;
        }
    }

    /* Case 4: hand-built spatial copy of the row above (code 0x120+161). */
    {
        replay_buffer_clear(&payload);
        for (i = 0; i < 8U; ++i) {
            put16(&payload, (100U + i) << 1U);
        }
        put16(&payload, 1U + ((0x120U + (8U * 19U + 9U)) << 7U) +
                            ((8U - 2U) << 1U));
        put16(&payload, EOF_WORD);
        if (emit(manifest, dir, "spatial", &payload, NULL, 8U, 2U) != 0) {
            goto done;
        }
    }

    /* Case 5: hand-built long-literal run (bit-packed 15-bit pixels). */
    {
        static const uint16_t pixels[5] = {
            0x0001U, 0x7FFFU, 0x2468U, 0x1357U, 0x4000U
        };
        uint64_t acc = 0U;
        unsigned nbits = 0U;

        replay_buffer_clear(&payload);
        put16(&payload, 1U + (0x1FU << 11U) + ((5U - 1U) << 1U));
        for (i = 0; i < 5U; ++i) {
            acc |= (uint64_t)pixels[i] << nbits;
            nbits += 15U;
            while (nbits >= 8U) {
                (void)replay_buffer_append_u8(&payload, (uint8_t)(acc & 0xFFU));
                acc >>= 8U;
                nbits -= 8U;
            }
        }
        if (nbits != 0U) {
            (void)replay_buffer_append_u8(&payload, (uint8_t)(acc & 0xFFU));
        }
        if ((payload.size & 1U) != 0U) {
            (void)replay_buffer_append_u8(&payload, 0U);
        }
        put16(&payload, EOF_WORD);
        if (emit(manifest, dir, "longlit", &payload, NULL, 5U, 1U) != 0) {
            goto done;
        }
    }

    /* Case 6: hand-built repeated-pixel run. */
    {
        replay_buffer_clear(&payload);
        put16(&payload, 1U + (0x1CCU << 7U) + ((8U - 2U) << 1U)); /* repeat 8 */
        put16(&payload, 0x2AAAU);                                  /* the pixel */
        put16(&payload, EOF_WORD);
        if (emit(manifest, dir, "repeat", &payload, NULL, 8U, 1U) != 0) {
            goto done;
        }
    }

    /* Case 7: a multi-frame "movie" from the real encoder -- a structured clip
       that pans right, so later frames lean on temporal copies of a previously
       *reconstructed* frame. Each frame is cross-checked against the module with
       the prior reconstruction as its previous, validating the encoder's
       inter-frame chain end to end. */
    {
        enum { MW = 24U, MH = 12U, MN = 5U };
        uint16_t framebuf[MW * MH];
        uint16_t recon[MW * MH];
        uint16_t previous[MW * MH];
        int have_previous = 0;
        unsigned n;

        for (n = 0; n < MN; ++n) {
            unsigned x, y;
            char stem[32];
            const uint16_t *prev_arg = have_previous ? previous : NULL;

            for (y = 0; y < MH; ++y) {
                for (x = 0; x < MW; ++x) {
                    unsigned band = ((x + n * 2U) / 3U) % 32U;

                    framebuf[y * MW + x] = (uint16_t)(
                        (band << 10) | (((band * 2U) & 31U) << 5) |
                        ((MH - y) & 31U));
                }
            }
            if (codec_movinglines_encode_frame(framebuf, prev_arg, MW, MH,
                                               &payload) != REPLAY_OK) {
                goto done;
            }
            snprintf(stem, sizeof(stem), "movie%u", n);
            if (emit(manifest, dir, stem, &payload, prev_arg, MW, MH) != 0) {
                goto done;
            }
            if (codec_movinglines_decode_frame(payload.data, payload.size,
                                               prev_arg, recon, MW, MH,
                                               NULL) != REPLAY_OK) {
                goto done;
            }
            memcpy(previous, recon, sizeof(previous));
            have_previous = 1;
        }
    }

    /* Case 8: the "repeat last frame" pad payload (mb_repeat_payload, codec 1)
       the muxer appends to fill a final chunk -- it must reproduce the previous
       frame unchanged on the module. */
    {
        uint16_t previous[128];
        unsigned k;

        for (k = 0; k < 128U; ++k) {
            previous[k] = (uint16_t)((k * 13U) & 0x7FFFU);
        }
        if (mb_repeat_payload(1U, 16U, 8U, &payload) != REPLAY_OK ||
            emit(manifest, dir, "padrepeat", &payload, previous, 16U, 8U) != 0) {
            goto done;
        }
    }

    result = EXIT_SUCCESS;

done:
    replay_buffer_free(&payload);
    if (fclose(manifest) != 0) {
        result = EXIT_FAILURE;
    }
    return result;
}
