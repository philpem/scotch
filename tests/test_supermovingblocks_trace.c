#include "test_common.h"

#include <string.h>

#include "replay/codec_supermovingblocks.h"

typedef struct {
    CodecSuperMovingBlocksDecodeEvent events[5];
    size_t count;
} TraceLog;

static void collect_event(const CodecSuperMovingBlocksDecodeEvent *event,
                          void *opaque)
{
    TraceLog *log = opaque;

    if (log->count < 5U) {
        log->events[log->count] = *event;
    }
    ++log->count;
}

int main(void)
{
    static const uint8_t payload[] = { UINT8_C(0x03), UINT8_C(0x00) };
    MbPixel previous_pixels[16];
    MbPixel decoded_pixels[16];
    MbFrame previous = { 4U, 4U, 4U, previous_pixels };
    MbFrame decoded = { 4U, 4U, 4U, decoded_pixels };
    TraceLog log = { { { 0 } }, 0U };
    size_t bits;
    size_t i;

    for (i = 0U; i < 16U; ++i) {
        previous_pixels[i] = (MbPixel){ (uint8_t)i, 3U, 5U };
    }
    CHECK(codec_supermovingblocks_verify_frame_traced(
              payload, sizeof(payload), &previous, &decoded, &bits, NULL,
              collect_event, &log) == REPLAY_OK);
    CHECK(bits == 10U);
    CHECK(log.count == 5U);
    for (i = 0U; i < 4U; ++i) {
        CHECK(log.events[i].size == 2U);
        CHECK(log.events[i].mode ==
              CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY);
        CHECK(log.events[i].bit_start == 2U + i * 2U);
        CHECK(log.events[i].bit_end == 4U + i * 2U);
    }
    CHECK(log.events[0].x == 0U && log.events[0].y == 0U);
    CHECK(log.events[1].x == 2U && log.events[1].y == 0U);
    CHECK(log.events[2].x == 0U && log.events[2].y == 2U);
    CHECK(log.events[3].x == 2U && log.events[3].y == 2U);
    CHECK(log.events[4].size == 4U);
    CHECK(log.events[4].mode == CODEC_SUPERMOVINGBLOCKS_MODE_SPLIT);
    CHECK(log.events[4].bit_start == 0U && log.events[4].bit_end == 10U);
    CHECK(memcmp(previous_pixels, decoded_pixels,
                 sizeof(previous_pixels)) == 0);
    return EXIT_SUCCESS;
}
