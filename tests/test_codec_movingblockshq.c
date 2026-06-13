#include "test_common.h"

#include <stdint.h>
#include <string.h>

#include "replay/codec_movingblockshq.h"
#include "replay/mb_motion.h"

static void fill_previous(MbPixel *pixels, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        pixels[index].y = (uint8_t)(index & 31U);
        pixels[index].u = (uint8_t)((index + 3U) & 31U);
        pixels[index].v = (uint8_t)((index + 7U) & 31U);
    }
}

static int same_pixel(const MbPixel *left, const MbPixel *right)
{
    return left->y == right->y && left->u == right->u &&
           left->v == right->v;
}

static int test_data4x4_zero_residual(void)
{
    static const uint8_t payload[] = {
        0x8dU, 0xa2U, 0xaaU, 0xaaU, 0xaaU, 0x0aU
    };
    ReplayBitReader reader;
    MbPredictor predictor = { 7U };
    MbPixel pixels[16];
    size_t index;

    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_movingblockshq_decode_data4x4(
              &reader, &predictor, pixels, 4U, NULL) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 44U);
    CHECK(predictor.luma == 7U);
    for (index = 0U; index < 16U; ++index) {
        CHECK(pixels[index].y == 7U);
        CHECK(pixels[index].u == 3U);
        CHECK(pixels[index].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_data2x2_wrap(void)
{
    /* opcode 2, U=0, V=0, then residuals 1,0,0,0. */
    static const uint8_t payload[] = { 0x02U, 0x70U, 0x15U };
    ReplayBitReader reader;
    MbPredictor predictor = { 31U };
    MbPixel pixels[4];

    replay_bitreader_init(&reader, payload, sizeof(payload));
    CHECK(codec_movingblockshq_decode_data2x2(
              &reader, &predictor, pixels, 2U, NULL) == REPLAY_OK);
    CHECK(replay_bitreader_position(&reader) == 21U);
    CHECK(predictor.luma == 0U);
    CHECK(pixels[0].y == 0U && pixels[1].y == 0U);
    CHECK(pixels[2].y == 0U && pixels[3].y == 0U);
    return EXIT_SUCCESS;
}

static int test_table_and_truncation(void)
{
    static const uint8_t truncated[] = { 0x8dU };
    ReplayBitReader reader;
    MbPredictor predictor = { 0U };
    MbPixel pixels[16];
    MbVerifyError error = { 0 };

    CHECK(mb_huffman_validate(&codec_movingblockshq_luma_huffman) ==
          REPLAY_OK);
    replay_bitreader_init(&reader, truncated, sizeof(truncated));
    CHECK(codec_movingblockshq_decode_data4x4(
              &reader, &predictor, pixels, 4U, &error) ==
          REPLAY_TRUNCATED_INPUT);
    CHECK(error.detail != NULL);
    return EXIT_SUCCESS;
}

static int test_complete_data_frame(void)
{
    static const uint8_t payload[] = {
        0x8dU, 0xa2U, 0xaaU, 0xaaU, 0xaaU, 0x0aU
    };
    MbPixel decoded_pixels[16];
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    size_t bits_consumed = 0U;
    size_t index;

    CHECK(codec_movingblockshq_verify_frame(
              payload, sizeof(payload), NULL, &decoded,
              &bits_consumed, NULL) == REPLAY_OK);
    CHECK(bits_consumed == 44U);
    for (index = 0U; index < 16U; ++index) {
        CHECK(decoded_pixels[index].y == 0U);
        CHECK(decoded_pixels[index].u == 3U);
        CHECK(decoded_pixels[index].v == 5U);
    }
    return EXIT_SUCCESS;
}

static int test_4x4_copy_modes(void)
{
    MbPixel previous_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbMotionVector motion = { -1, 0, 0 };
    unsigned row;

    fill_previous(previous_pixels, 32U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);

    /* First parent is stationary; second copies a temporal offset. */
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 2U, 2U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_4X4, &motion) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 4U; ++row) {
        unsigned column;

        for (column = 0U; column < 4U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 8U + column],
                             &previous_pixels[row * 8U + column]));
            CHECK(same_pixel(&decoded_pixels[row * 8U + 4U + column],
                             &previous_pixels[row * 8U + 3U + column]));
        }
    }

    /* Rebuild the second parent as a spatial copy of the first. */
    replay_buffer_clear(&payload);
    replay_bitwriter_init(&writer, &payload);
    motion.dx = -4;
    motion.dy = 0;
    motion.spatial = 1;
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 2U, 2U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_4X4, &motion) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 4U; ++row) {
        unsigned column;

        for (column = 0U; column < 4U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 8U + 4U + column],
                             &decoded_pixels[row * 8U + column]));
        }
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_split_copy_modes(void)
{
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    MbMotionVector temporal = { -1, 0, 0 };
    MbMotionVector spatial = { -2, 0, 1 };
    unsigned row;

    fill_previous(previous_pixels, 16U);
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);

    CHECK(replay_bitwriter_write(&writer, 3U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_2X2, &temporal) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 0U, 2U) == REPLAY_OK);
    CHECK(replay_bitwriter_write(&writer, 1U, 1U) == REPLAY_OK);
    CHECK(mb_motion_write_format19(
              &writer, MB_MOTION_BLOCK_2X2, &spatial) == REPLAY_OK);
    CHECK(replay_bitwriter_flush_zero(&writer) == REPLAY_OK);

    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded,
              NULL, NULL) == REPLAY_OK);
    for (row = 0U; row < 2U; ++row) {
        unsigned column;

        for (column = 0U; column < 2U; ++column) {
            CHECK(same_pixel(&decoded_pixels[row * 4U + column],
                             &previous_pixels[row * 4U + column]));
            CHECK(same_pixel(&decoded_pixels[row * 4U + 2U + column],
                             &previous_pixels[row * 4U + 1U + column]));
            CHECK(same_pixel(&decoded_pixels[(row + 2U) * 4U + column],
                             &previous_pixels[(row + 2U) * 4U + column]));
            CHECK(same_pixel(
                &decoded_pixels[(row + 2U) * 4U + 2U + column],
                &decoded_pixels[(row + 2U) * 4U + column]));
        }
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

/* Encode a source block, decode it back, and confirm lossless luma, the chosen
 * average chroma, and that the encoder's reconstruction and predictor match the
 * decoder's exactly. */
static int test_encode_roundtrip(void)
{
    MbPixel source[16];
    MbPixel recon[16];
    MbPixel decoded[16];
    MbPredictor enc = { 7U };
    MbPredictor dec = { 7U };
    ReplayBuffer payload;
    ReplayBitWriter writer;
    ReplayBitReader reader;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        source[i].y = (uint8_t)((i * 3U + 5U) & 31U); /* varying luma */
        source[i].u = 10U;                            /* constant chroma */
        source[i].v = 20U;
    }
    replay_buffer_init(&payload);
    replay_bitwriter_init(&writer, &payload);
    CHECK(codec_movingblockshq_encode_data4x4(&writer, source, recon, 4U,
                                              &enc) == REPLAY_OK);
    replay_bitwriter_flush_zero(&writer);
    replay_bitreader_init(&reader, payload.data, payload.size);
    CHECK(codec_movingblockshq_decode_data4x4(&reader, &dec, decoded, 4U,
                                              NULL) == REPLAY_OK);
    for (i = 0U; i < 16U; ++i) {
        CHECK(decoded[i].y == source[i].y); /* lossless luma */
        CHECK(decoded[i].u == 10U && decoded[i].v == 20U);
        CHECK(same_pixel(&decoded[i], &recon[i])); /* encoder recon == decode */
    }
    CHECK(enc.luma == dec.luma);
    replay_buffer_free(&payload);

    /* 2x2 with non-uniform chroma exercises the average. */
    {
        MbPixel src2[4] = {
            { 3U, 8U, 21U }, { 9U, 9U, 22U },
            { 30U, 11U, 24U }, { 1U, 12U, 25U }
        };
        MbPixel rec2[4];
        MbPixel dec2[4];
        MbPredictor e2 = { 15U };
        MbPredictor d2 = { 15U };
        uint8_t eu = (uint8_t)((8U + 9U + 11U + 12U + 2U) / 4U);
        uint8_t ev = (uint8_t)((21U + 22U + 24U + 25U + 2U) / 4U);

        replay_buffer_init(&payload);
        replay_bitwriter_init(&writer, &payload);
        CHECK(codec_movingblockshq_encode_data2x2(&writer, src2, rec2, 2U,
                                                  &e2) == REPLAY_OK);
        replay_bitwriter_flush_zero(&writer);
        replay_bitreader_init(&reader, payload.data, payload.size);
        CHECK(codec_movingblockshq_decode_data2x2(&reader, &d2, dec2, 2U,
                                                  NULL) == REPLAY_OK);
        for (i = 0U; i < 4U; ++i) {
            CHECK(dec2[i].y == src2[i].y);
            CHECK(dec2[i].u == eu && dec2[i].v == ev);
            CHECK(same_pixel(&dec2[i], &rec2[i]));
        }
        CHECK(e2.luma == d2.luma);
        replay_buffer_free(&payload);
    }
    return EXIT_SUCCESS;
}

static int test_data_frame_encode_roundtrip(void)
{
    MbPixel source_pixels[64];
    MbPixel recon_pixels[64];
    MbPixel decoded_pixels[64];
    MbFrame source = { 8U, 8U, 8U, source_pixels };
    MbFrame reconstructed = { 8U, 8U, 8U, recon_pixels };
    MbFrame decoded = { 8U, 8U, 8U, decoded_pixels };
    ReplayBuffer payload;
    size_t bits_written = 0U;
    size_t consumed = 0U;
    unsigned index;

    /* A varied 5-bit YUV555 source spanning four 4x4 blocks. */
    for (index = 0U; index < 64U; ++index) {
        source_pixels[index].y = (uint8_t)((index * 7U) & 31U);
        source_pixels[index].u = (uint8_t)((index * 3U + 1U) & 31U);
        source_pixels[index].v = (uint8_t)((index * 5U + 2U) & 31U);
    }
    memset(recon_pixels, 0xAA, sizeof(recon_pixels));
    replay_buffer_init(&payload);

    CHECK(codec_movingblockshq_encode_data_frame(
              &source, &payload, &reconstructed, &bits_written) == REPLAY_OK);
    CHECK(bits_written > 0U);
    CHECK(bits_written <= payload.size * 8U);

    /* A data-only frame is independently decodable, so previous is NULL. */
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, NULL, &decoded,
              &consumed, NULL) == REPLAY_OK);
    CHECK(consumed == bits_written);

    /* The decoder must reproduce exactly what the encoder reconstructed: luma
       is lossless and chroma is the block average both sides computed. */
    for (index = 0U; index < 64U; ++index) {
        CHECK(same_pixel(&decoded_pixels[index], &recon_pixels[index]));
        CHECK(decoded_pixels[index].y == source_pixels[index].y);
    }
    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int frames_equal(const MbFrame *a, const MbFrame *b)
{
    unsigned y;

    for (y = 0U; y < a->height; ++y) {
        unsigned x;
        for (x = 0U; x < a->width; ++x) {
            if (!same_pixel(&a->pixels[(size_t)y * a->stride + x],
                            &b->pixels[(size_t)y * b->stride + x])) {
                return 0;
            }
        }
    }
    return 1;
}

static int test_encode_frame_copy_modes(void)
{
    MbPixel source_pixels[32];
    MbPixel previous_pixels[32];
    MbPixel recon_pixels[32];
    MbPixel decoded_pixels[32];
    MbFrame source = { 8U, 4U, 8U, source_pixels };
    MbFrame previous = { 8U, 4U, 8U, previous_pixels };
    MbFrame reconstructed = { 8U, 4U, 8U, recon_pixels };
    MbFrame decoded = { 8U, 4U, 8U, decoded_pixels };
    CodecMovingBlocksHqEncodeStats stats;
    ReplayBuffer payload;
    unsigned y;

    /* Spatial key frame: the right 4x4 duplicates the left, so the left is a
       data block and the right a spatial copy of it. */
    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 0U; x < 4U; ++x) {
            MbPixel pixel = { (uint8_t)((y * 4U + x) & 31U), 9U, 21U };
            source_pixels[y * 8U + x] = pixel;
            source_pixels[y * 8U + x + 4U] = pixel;
        }
    }
    replay_buffer_init(&payload);
    {
        CodecMovingBlocksHqEncodeOptions options = { 0, 0, 1, 0, 0U };
        CHECK(codec_movingblockshq_encode_frame(
                  &source, NULL, &options, &payload, &reconstructed, &stats) ==
              REPLAY_OK);
    }
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(stats.spatial4x4_blocks == 1U);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, NULL, &decoded, NULL, NULL) ==
          REPLAY_OK);
    CHECK(frames_equal(&decoded, &reconstructed));

    /* Stationary inter frame: the left 4x4 equals the previous frame (a
       stationary copy); the right differs and falls back to a data block. */
    fill_previous(previous_pixels, 32U);
    for (y = 0U; y < 32U; ++y) {
        source_pixels[y] = previous_pixels[y];
    }
    for (y = 0U; y < 4U; ++y) {
        unsigned x;
        for (x = 4U; x < 8U; ++x) {
            source_pixels[y * 8U + x].y =
                (uint8_t)((source_pixels[y * 8U + x].y + 9U) & 31U);
        }
    }
    replay_buffer_clear(&payload);
    {
        CodecMovingBlocksHqEncodeOptions options = { 1, 0, 0, 0, 0U };
        CHECK(codec_movingblockshq_encode_frame(
                  &source, &previous, &options, &payload, &reconstructed,
                  &stats) == REPLAY_OK);
    }
    CHECK(stats.stationary4x4_blocks == 1U);
    CHECK(stats.data4x4_blocks == 1U);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded, NULL, NULL) ==
          REPLAY_OK);
    CHECK(frames_equal(&decoded, &reconstructed));

    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

static int test_encode_frame_split(void)
{
    MbPixel source_pixels[16];
    MbPixel previous_pixels[16];
    MbPixel recon_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame source = { 4U, 4U, 4U, source_pixels };
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame reconstructed = { 4U, 4U, 4U, recon_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    CodecMovingBlocksHqEncodeOptions options = { 1, 0, 0, 1, 0U };
    CodecMovingBlocksHqEncodeStats stats;
    ReplayBuffer payload;
    unsigned i;

    /* High-frequency luma that is expensive to data-code but matches the
       previous frame exactly, so three quadrants copy for free. */
    for (i = 0U; i < 16U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)((i & 1U) ? 31U : 0U), 4U, 6U };
        source_pixels[i] = previous_pixels[i];
    }
    /* Change the bottom-right 2x2 quadrant so it cannot copy. */
    source_pixels[10].y = 7U;
    source_pixels[11].y = 18U;
    source_pixels[14].y = 25U;
    source_pixels[15].y = 12U;

    replay_buffer_init(&payload);
    CHECK(codec_movingblockshq_encode_frame(
              &source, &previous, &options, &payload, &reconstructed, &stats) ==
          REPLAY_OK);
    CHECK(stats.split4x4_blocks == 1U);
    CHECK(stats.stationary2x2_blocks == 3U);
    CHECK(stats.data2x2_blocks == 1U);
    CHECK(stats.data4x4_blocks == 0U);
    CHECK(codec_movingblockshq_verify_frame(
              payload.data, payload.size, &previous, &decoded, NULL, NULL) ==
          REPLAY_OK);
    CHECK(frames_equal(&decoded, &reconstructed));

    replay_buffer_free(&payload);
    return EXIT_SUCCESS;
}

int main(void)
{
    CHECK(test_data4x4_zero_residual() == EXIT_SUCCESS);
    CHECK(test_encode_roundtrip() == EXIT_SUCCESS);
    CHECK(test_data_frame_encode_roundtrip() == EXIT_SUCCESS);
    CHECK(test_encode_frame_copy_modes() == EXIT_SUCCESS);
    CHECK(test_encode_frame_split() == EXIT_SUCCESS);
    CHECK(test_data2x2_wrap() == EXIT_SUCCESS);
    CHECK(test_table_and_truncation() == EXIT_SUCCESS);
    CHECK(test_complete_data_frame() == EXIT_SUCCESS);
    CHECK(test_4x4_copy_modes() == EXIT_SUCCESS);
    CHECK(test_split_copy_modes() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
