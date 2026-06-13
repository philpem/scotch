#include "replay/replay_sound.h"

/*
 * Acorn's exact VIDC 8-bit "exponential" decode table (E format), transcribed
 * from RiscOS_2003/RiscOS/Sources/Audio/SoundFile/s/e8to16 (ELogToLinTable).
 * Each entry maps an 8-bit code to its reconstructed signed 16-bit sample. The
 * format is sign-magnitude logarithmic: even codes are non-negative, odd codes
 * are the matching negative magnitude, and the step size doubles up the curve.
 *
 * Encoding is the nearest-match inverse of this table, so a round trip through
 * the genuine player's decoder reproduces these exact sample values.
 */
static const int16_t e_log_to_lin[256] = {
         0,      0,      8,     -8,     16,    -16,     24,    -24,
        32,    -32,     40,    -40,     48,    -48,     56,    -56,
        64,    -64,     72,    -72,     80,    -80,     88,    -88,
        96,    -96,    104,   -104,    112,   -112,    120,   -120,
       128,   -128,    144,   -144,    160,   -160,    176,   -176,
       192,   -192,    208,   -208,    224,   -224,    240,   -240,
       256,   -256,    272,   -272,    288,   -288,    304,   -304,
       320,   -320,    336,   -336,    352,   -352,    368,   -368,
       384,   -384,    416,   -416,    448,   -448,    480,   -480,
       512,   -512,    544,   -544,    576,   -576,    608,   -608,
       640,   -640,    672,   -672,    704,   -704,    736,   -736,
       768,   -768,    800,   -800,    832,   -832,    864,   -864,
       896,   -896,    960,   -960,   1024,  -1024,   1088,  -1088,
      1152,  -1152,   1216,  -1216,   1280,  -1280,   1344,  -1344,
      1408,  -1408,   1472,  -1472,   1536,  -1536,   1600,  -1600,
      1664,  -1664,   1728,  -1728,   1792,  -1792,   1856,  -1856,
      1920,  -1920,   2048,  -2048,   2176,  -2176,   2304,  -2304,
      2432,  -2432,   2560,  -2560,   2688,  -2688,   2816,  -2816,
      2944,  -2944,   3072,  -3072,   3200,  -3200,   3328,  -3328,
      3456,  -3456,   3584,  -3584,   3712,  -3712,   3840,  -3840,
      3968,  -3968,   4224,  -4224,   4480,  -4480,   4736,  -4736,
      4992,  -4992,   5248,  -5248,   5504,  -5504,   5760,  -5760,
      6016,  -6016,   6272,  -6272,   6528,  -6528,   6784,  -6784,
      7040,  -7040,   7296,  -7296,   7552,  -7552,   7808,  -7808,
      8064,  -8064,   8576,  -8576,   9088,  -9088,   9600,  -9600,
     10112, -10112,  10624, -10624,  11136, -11136,  11648, -11648,
     12160, -12160,  12672, -12672,  13184, -13184,  13696, -13696,
     14208, -14208,  14720, -14720,  15232, -15232,  15744, -15744,
     16256, -16256,  17280, -17280,  18304, -18304,  19328, -19328,
     20352, -20352,  21376, -21376,  22400, -22400,  23424, -23424,
     24448, -24448,  25472, -25472,  26496, -26496,  27520, -27520,
     28544, -28544,  29568, -29568,  30592, -30592,  31616, -31616
};

int16_t replay_sound_vidc_e8_to_s16(uint8_t code)
{
    return e_log_to_lin[code];
}

uint8_t replay_sound_vidc_e8_from_s16(int16_t sample)
{
    /* The two sign families are interleaved (even = non-negative, odd = the
     * negated magnitude), so only the matching 128 codes are searched. */
    unsigned start = sample < 0 ? 1U : 0U;
    unsigned best = start;
    int32_t best_distance = INT32_MAX;
    unsigned code;

    for (code = start; code < 256U; code += 2U) {
        int32_t delta = (int32_t)sample - (int32_t)e_log_to_lin[code];

        if (delta < 0) {
            delta = -delta;
        }
        if (delta < best_distance) {
            best_distance = delta;
            best = code;
            if (delta == 0) {
                break;
            }
        }
    }
    return (uint8_t)best;
}

ReplayStatus replay_sound_encode(ReplaySoundFormat format,
                                 const int16_t *samples, size_t sample_count,
                                 ReplayBuffer *out)
{
    size_t i;

    if (out == NULL || (samples == NULL && sample_count != 0U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (i = 0U; i < sample_count; ++i) {
        int16_t sample = samples[i];
        ReplayStatus status;

        switch (format) {
        case REPLAY_SOUND_VIDC_E8:
            status = replay_buffer_append_u8(
                out, replay_sound_vidc_e8_from_s16(sample));
            break;
        case REPLAY_SOUND_SIGNED_8:
            /* High byte of the signed 16-bit sample. */
            status = replay_buffer_append_u8(
                out, (uint8_t)((uint16_t)sample >> 8));
            break;
        case REPLAY_SOUND_SIGNED_16: {
            uint8_t bytes[2] = {
                (uint8_t)((uint16_t)sample & 0xFFU),
                (uint8_t)((uint16_t)sample >> 8)
            };
            status = replay_buffer_append(out, bytes, sizeof(bytes));
            break;
        }
        default:
            return REPLAY_INVALID_ARGUMENT;
        }
        if (status != REPLAY_OK) {
            return status;
        }
    }
    return REPLAY_OK;
}

/*
 * IMA/DVI ADPCM, the canonical reference codec (Jack Jansen, 7-Jul-92) Acorn's
 * Join uses. The two tables are part of the format.
 */
static const int8_t adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
static const int16_t adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2747, 3022,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767
};

static uint8_t adpcm_encode_sample(int sample, ReplaySoundAdpcmState *state)
{
    int step = adpcm_step_table[state->step_index];
    int diff = sample - state->predicted;
    int code = 0;
    int reconstructed = step >> 3;
    int predicted;
    int index;

    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
        reconstructed += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        diff -= step >> 1;
        reconstructed += step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        reconstructed += step >> 2;
    }
    predicted = state->predicted +
                ((code & 8) ? -reconstructed : reconstructed);
    if (predicted > 32767) {
        predicted = 32767;
    } else if (predicted < -32768) {
        predicted = -32768;
    }
    index = state->step_index + adpcm_index_table[code & 0x0F];
    if (index < 0) {
        index = 0;
    } else if (index > 88) {
        index = 88;
    }
    state->predicted = (int16_t)predicted;
    state->step_index = (int8_t)index;
    return (uint8_t)(code & 0x0F);
}

ReplayStatus replay_sound_adpcm_encode(const int16_t *samples, size_t count,
                                       ReplaySoundAdpcmState *state,
                                       ReplayBuffer *out)
{
    size_t i;

    if (out == NULL || state == NULL || (samples == NULL && count != 0U)) {
        return REPLAY_INVALID_ARGUMENT;
    }
    for (i = 0U; i < count; i += 2U) {
        uint8_t low = adpcm_encode_sample(samples[i], state);
        uint8_t high = 0U;
        ReplayStatus status;

        if (i + 1U < count) {
            high = adpcm_encode_sample(samples[i + 1U], state);
        }
        status = replay_buffer_append_u8(out, (uint8_t)(low | (high << 4)));
        if (status != REPLAY_OK) {
            return status;
        }
    }
    return REPLAY_OK;
}

ReplayStatus replay_sound_adpcm_write_header(const ReplaySoundAdpcmState *state,
                                             ReplayBuffer *out)
{
    uint8_t header[4] = {
        (uint8_t)((uint16_t)state->predicted & 0xFFU),
        (uint8_t)((uint16_t)state->predicted >> 8),
        (uint8_t)state->step_index,
        0U
    };

    if (out == NULL || state == NULL) {
        return REPLAY_INVALID_ARGUMENT;
    }
    return replay_buffer_append(out, header, sizeof(header));
}

void replay_sound_adpcm_decode(const uint8_t *nibbles, size_t count,
                               ReplaySoundAdpcmState *state,
                               int16_t *out_samples)
{
    size_t i;

    for (i = 0U; i < count; ++i) {
        unsigned code = (i & 1U) == 0U ? (nibbles[i / 2U] & 0x0FU)
                                       : (nibbles[i / 2U] >> 4U);
        int step = adpcm_step_table[state->step_index];
        int diff = step >> 3;
        int predicted;
        int index;

        if (code & 4U) {
            diff += step;
        }
        if (code & 2U) {
            diff += step >> 1;
        }
        if (code & 1U) {
            diff += step >> 2;
        }
        predicted = state->predicted + ((code & 8U) ? -diff : diff);
        if (predicted > 32767) {
            predicted = 32767;
        } else if (predicted < -32768) {
            predicted = -32768;
        }
        index = state->step_index + adpcm_index_table[code & 0x0FU];
        if (index < 0) {
            index = 0;
        } else if (index > 88) {
            index = 88;
        }
        state->predicted = (int16_t)predicted;
        state->step_index = (int8_t)index;
        out_samples[i] = (int16_t)predicted;
    }
}

unsigned replay_sound_format_bits(ReplaySoundFormat format)
{
    return format == REPLAY_SOUND_SIGNED_16 ? 16U : 8U;
}

const char *replay_sound_format_label(ReplaySoundFormat format)
{
    /* Text follows the bits-per-sample number; "LIN" selects linear decoding,
     * its absence selects the exponential decoder. */
    switch (format) {
    case REPLAY_SOUND_SIGNED_8:
    case REPLAY_SOUND_SIGNED_16:
        return "bit linear signed";
    case REPLAY_SOUND_VIDC_E8:
    default:
        return "bits per sample (exponential)";
    }
}
