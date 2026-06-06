#include "test_common.h"

#include "replay/mb_codec.h"

int main(void)
{
    const MbCodec *codec;

    codec = mb_codec_find(19U);
    CHECK(codec == &codec_supermovingblocks);
    CHECK(codec->working_format == MB_WORK_6Y5UV);
    CHECK(codec->y_bits == 6U);
    CHECK(codec->u_bits == 5U);
    CHECK(codec->luma_huffman != NULL);
    CHECK(codec->encoder_implemented);
    CHECK(codec->verifier_implemented);

    CHECK(mb_codec_find(7U) == &codec_movingblocks);
    CHECK(mb_codec_find(17U) == &codec_movingblockshq);
    CHECK(mb_codec_find(20U) == &codec_movingblocksbeta);
    CHECK(mb_codec_find(18U) == NULL);
    return EXIT_SUCCESS;
}
