/* Unit test for the NUT muxer: mux a tiny two-stream movie to a temp file and
 * check the structure the muxer is responsible for -- the file id string, the
 * startcodes, the variable-length integer coding, and the packet CRC footers.
 * This guards the bit-twiddling without needing ffmpeg; the ffmpeg-gated
 * round-trip test (test_transcode_nut.sh) checks demuxer acceptance end to end.
 */

#include "replay/replay_nut.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_STARTCODE      (0x7A561F5F04ADULL + (((uint64_t)(('N' << 8) + 'M')) << 48))
#define STREAM_STARTCODE    (0x11405BF2F9DBULL + (((uint64_t)(('N' << 8) + 'S')) << 48))
#define SYNCPOINT_STARTCODE (0xE4ADEECA4569ULL + (((uint64_t)(('N' << 8) + 'K')) << 48))

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
} while (0)

/* Mirror of the muxer's CRC so the test is an independent re-derivation. */
static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t crc = 0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << 24;
        for (k = 0; k < 8; k++)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04C11DB7u : 0u);
    }
    return crc;
}

/* Read a NUT variable-length unsigned value at *pos; advance *pos. */
static uint64_t read_v(const uint8_t *d, size_t len, size_t *pos)
{
    uint64_t v = 0;
    while (*pos < len) {
        uint8_t b = d[(*pos)++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80u))
            break;
    }
    return v;
}

static uint64_t read_be64(const uint8_t *d)
{
    uint64_t v = 0;
    int k;
    for (k = 0; k < 8; k++)
        v = (v << 8) | d[k];
    return v;
}

static uint32_t read_le32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16)
           | ((uint32_t)d[3] << 24);
}

static size_t count_startcode(const uint8_t *d, size_t len, uint64_t sc)
{
    size_t i, n = 0;
    for (i = 0; i + 8 <= len; i++)
        if (read_be64(d + i) == sc)
            n++;
    return n;
}

int main(void)
{
    const char *path = "test_nut_mux.tmp";
    uint8_t vframe[4 * 4 * 3];
    uint8_t aframe[64];
    ReplayNutStream streams[2];
    ReplayNutMuxer *m;
    char err[128];
    FILE *f;
    uint8_t *buf;
    long sz;
    size_t len, pos;
    uint64_t fwd;
    const char *id = "nut/multimedia container";

    memset(vframe, 0x40, sizeof vframe);
    memset(aframe, 0x11, sizeof aframe);

    memset(streams, 0, sizeof streams);
    streams[0].cls = REPLAY_NUT_VIDEO;
    streams[0].fourcc[0] = 'R'; streams[0].fourcc[1] = 'G';
    streams[0].fourcc[2] = 'B'; streams[0].fourcc[3] = 24;
    streams[0].tb_num = 1; streams[0].tb_den = 25;
    streams[0].width = 4; streams[0].height = 4;
    streams[1].cls = REPLAY_NUT_AUDIO;
    streams[1].fourcc[0] = 'P'; streams[1].fourcc[1] = 'S';
    streams[1].fourcc[2] = 'D'; streams[1].fourcc[3] = 16;
    streams[1].tb_num = 1; streams[1].tb_den = 8000;
    streams[1].sample_rate = 8000; streams[1].channels = 1;

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot create %s\n", path);
        return 2;
    }
    m = replay_nut_open(f, streams, 2, err, sizeof err);
    CHECK(m != NULL, "replay_nut_open");
    if (m == NULL) {
        fclose(f);
        return 2;
    }
    CHECK(replay_nut_write_frame(m, 1, 0, 1, aframe, sizeof aframe) == REPLAY_OK,
          "write audio frame");
    CHECK(replay_nut_write_frame(m, 0, 0, 1, vframe, sizeof vframe) == REPLAY_OK,
          "write video frame");
    CHECK(replay_nut_close(m) == REPLAY_OK, "close");
    fclose(f);

    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return 2;
    }
    rewind(f);
    len = (size_t)sz;
    buf = malloc(len);
    if (buf == NULL || fread(buf, 1, len, f) != len) {
        fprintf(stderr, "read error\n");
        return 2;
    }
    fclose(f);

    /* file_id_string + NUL. */
    CHECK(len > 25, "file long enough");
    CHECK(memcmp(buf, id, 24) == 0 && buf[24] == 0, "file_id_string");

    /* The first packet is the main header. */
    pos = 25;
    CHECK(len >= pos + 8 && read_be64(buf + pos) == MAIN_STARTCODE,
          "main startcode");
    pos += 8;
    fwd = read_v(buf, len, &pos);
    CHECK(fwd >= 4 && pos + fwd <= len, "main forward_ptr in range");
    if (fwd >= 4 && pos + fwd <= len) {
        size_t payload_len = (size_t)fwd - 4;
        uint32_t stored = read_le32(buf + pos + payload_len);
        CHECK(crc32(buf + pos, payload_len) == stored, "main footer CRC");
        /* version=3, stream_count=2, max_distance, time_base_count=2 */
        size_t hp = pos;
        CHECK(read_v(buf, len, &hp) == 3, "version 3");
        CHECK(read_v(buf, len, &hp) == 2, "stream_count 2");
        (void)read_v(buf, len, &hp);                 /* max_distance */
        CHECK(read_v(buf, len, &hp) == 2, "time_base_count 2");
    }

    /* Two stream headers, at least one syncpoint, and frame_code 0 frames. */
    CHECK(count_startcode(buf, len, STREAM_STARTCODE) == 2, "two stream headers");
    CHECK(count_startcode(buf, len, SYNCPOINT_STARTCODE) >= 1, "a syncpoint");

    free(buf);
    remove(path);

    if (failures == 0)
        printf("test_nut_mux: OK\n");
    return failures == 0 ? 0 : 1;
}
