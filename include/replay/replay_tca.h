#ifndef REPLAY_TCA_H
#define REPLAY_TCA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decoder for Iota Software's "The Complete Animator" (TCA / ACEF / IotaFilm)
 * films — Replay video format 500. It operates on the IotaFilm bytes (the `ACEF`
 * chunk followed by `PALE` etc.), whether that is a bare `.tca` file or the film
 * embedded in a Replay container (in which case start at the video chunk's
 * file_offset — a type-500 movie embeds the whole film there contiguously).
 *
 * Frames are decoded to 8-bit palette indices plus a 256-entry RGB palette;
 * RISC OS 8bpp screen modes (28 / 21) are supported. The decode (variable-width
 * LZW, RLE or raw, with optional inter-frame XOR delta) follows Euclid_Expand;
 * see docs/spec/tca-type500.md.
 */

typedef struct ReplayTca ReplayTca;

/* Open over the IotaFilm bytes. Parses the ACEF film header and the PALE palette.
 * Returns NULL on error (message written to `err`). */
ReplayTca *replay_tca_open(const uint8_t *data, size_t len,
                           char *err, size_t errlen);
void replay_tca_close(ReplayTca *t);

unsigned replay_tca_width(const ReplayTca *t);
unsigned replay_tca_height(const ReplayTca *t);
unsigned replay_tca_frame_count(const ReplayTca *t);
/* Output bits per pixel: 8 for the palettised modes (output is 8-bit indices to
 * the palette), or 16 for a direct-colour film (output is packed RGB555, red in
 * the low bits). New-format RISC OS sprite mode words select the depth. */
unsigned replay_tca_bpp(const ReplayTca *t);
/* 256 * 3 interleaved RGB bytes (unused for 16bpp films). */
const uint8_t *replay_tca_palette(const ReplayTca *t);

/* Decode the next frame, in order, into `out`: width*height bytes of 8-bit
 * palette indices for an 8bpp film, or width*height*2 bytes of packed RGB555
 * (red low) for a 16bpp film (see replay_tca_bpp). Returns 1 on a decoded frame,
 * 0 at end of film, -1 on error (message in `err`). Frames must be taken in
 * order — Delta films carry state. */
int replay_tca_next_frame(ReplayTca *t, uint8_t *out, char *err, size_t errlen);

/* Decode the Iota soundtrack from the IotaFilm `data`: the `SOUN` chunk's sample
 * library (WAV1 = 8-bit VIDC-logarithmic, or WAV2 = 4-bit IMA ADPCM) to signed
 * 16-bit mono PCM. Returns a malloc'd sample buffer (caller frees) and sets
 * *out_count to the sample count, or NULL if there is no decodable soundtrack
 * (message in `err`). Independent of replay_tca_open. */
int16_t *replay_tca_decode_audio(const uint8_t *data, size_t len,
                                 size_t *out_count, char *err, size_t errlen);

#endif /* REPLAY_TCA_H */
