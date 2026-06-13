#include "test_common.h"

#include "replay/mb_encode.h"

/* The motion bit-cost models used as the lowest-error tie-break. */
static int test_motion_bits_format19(void)
{
    MbMotionVector m;

    m = (MbMotionVector){ 0, 0, 0 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_4X4) == 7U); /* 5 body + 2 */
    m = (MbMotionVector){ 0, -2, 0 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_4X4) == 8U); /* mag2 */
    m = (MbMotionVector){ 3, 0, 0 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_4X4) == 9U); /* mag3 */
    m = (MbMotionVector){ 8, 8, 0 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_4X4) == 12U); /* far */
    m = (MbMotionVector){ -4, 0, 1 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_4X4) == 9U); /* spatial */
    m = (MbMotionVector){ 0, 0, 0 };
    CHECK(mb_encode_motion_bits(&m, MB_MOTION_BLOCK_2X2) == 6U); /* +1 prefix */
    return EXIT_SUCCESS;
}

static int test_motion_bits_format7(void)
{
    MbMotionVector m;

    /* 4x4: 2-bit move opcode + move code. */
    m = (MbMotionVector){ 0, 0, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 4U); /* `00` */
    m = (MbMotionVector){ -1, -1, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 7U); /* d1 */
    m = (MbMotionVector){ -2, 0, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 8U); /* d2 */
    m = (MbMotionVector){ 3, 0, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 10U); /* d3 */
    m = (MbMotionVector){ 4, 0, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 10U); /* d4 */
    m = (MbMotionVector){ -4, 0, 1 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_4X4) == 10U); /* sp */

    /* 2x2: 1-bit move opcode. */
    m = (MbMotionVector){ 0, 0, 0 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_2X2) == 3U);
    m = (MbMotionVector){ -2, -1, 1 };
    CHECK(mb_encode_motion_bits_format7(&m, MB_MOTION_BLOCK_2X2) == 9U);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_motion_bits_format19() == EXIT_SUCCESS);
    CHECK(test_motion_bits_format7() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
