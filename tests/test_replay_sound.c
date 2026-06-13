#include "test_common.h"

#include <stdint.h>

#include "replay/replay_buffer.h"
#include "replay/replay_sound.h"

int main(void)
{
    ReplayBuffer out;
    unsigned code;
    int prev_pos;
    int prev_neg;
    int i;

    /* Every E8 code must round-trip exactly through encode(decode(code)): the
     * decoded sample re-encodes to a code with the identical sample value. */
    for (code = 0U; code < 256U; ++code) {
        int16_t decoded = replay_sound_vidc_e8_to_s16((uint8_t)code);
        uint8_t re = replay_sound_vidc_e8_from_s16(decoded);

        CHECK(replay_sound_vidc_e8_to_s16(re) == decoded);
    }

    /* Silence maps to code 0 (value 0). */
    CHECK(replay_sound_vidc_e8_from_s16(0) == 0U);

    /* The companding is monotonic: rising magnitude never decreases the decoded
     * magnitude, and the sign of the decoded value tracks the input. */
    prev_pos = -1;
    prev_neg = 1;
    for (i = 0; i <= 32767; i += 137) {
        int16_t decoded_pos = replay_sound_vidc_e8_to_s16(
            replay_sound_vidc_e8_from_s16((int16_t)i));
        int16_t decoded_neg = replay_sound_vidc_e8_to_s16(
            replay_sound_vidc_e8_from_s16((int16_t)(-i - 1)));

        CHECK(decoded_pos >= 0 && decoded_pos >= prev_pos);
        CHECK(decoded_neg <= 0 && decoded_neg <= prev_neg);
        /* Within the table range nearest-match error is half the local step
         * (<= 512); above the 31616 maximum the code saturates. */
        if (i <= 31616) {
            CHECK(decoded_pos - i <= 512 && i - decoded_pos <= 512);
        } else {
            CHECK(decoded_pos == 31616);
        }
        prev_pos = decoded_pos;
        prev_neg = decoded_neg;
    }

    /* Encoded stream lengths: E8/signed-8 are one byte per sample, signed-16
     * is two little-endian bytes per sample. */
    {
        static const int16_t samples[4] = { 0, 256, -256, 32767 };

        replay_buffer_init(&out);
        CHECK(replay_sound_encode(REPLAY_SOUND_VIDC_E8, samples, 4U, &out) ==
              REPLAY_OK);
        CHECK(out.size == 4U);

        replay_buffer_clear(&out);
        CHECK(replay_sound_encode(REPLAY_SOUND_SIGNED_8, samples, 4U, &out) ==
              REPLAY_OK);
        CHECK(out.size == 4U);
        /* 256 >> 8 == 1, -256 >> 8 == 0xFF. */
        CHECK(out.data[0] == 0x00U);
        CHECK(out.data[1] == 0x01U);
        CHECK(out.data[2] == 0xFFU);

        replay_buffer_clear(&out);
        CHECK(replay_sound_encode(REPLAY_SOUND_SIGNED_16, samples, 4U, &out) ==
              REPLAY_OK);
        CHECK(out.size == 8U);
        /* 256 little-endian = 00 01; -256 = 00 FF. */
        CHECK(out.data[2] == 0x00U && out.data[3] == 0x01U);
        CHECK(out.data[4] == 0x00U && out.data[5] == 0xFFU);
        replay_buffer_free(&out);
    }

    CHECK(replay_sound_format_bits(REPLAY_SOUND_SIGNED_16) == 16U);
    CHECK(replay_sound_format_bits(REPLAY_SOUND_VIDC_E8) == 8U);
    return EXIT_SUCCESS;
}
