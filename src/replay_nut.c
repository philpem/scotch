/* NUT container muxer. See include/replay/replay_nut.h for the design notes.
 *
 * Field layout follows ffmpeg's libavformat/nutenc.c so its demuxer accepts the
 * stream verbatim:
 *   - file_id_string, then a MAIN header, one STREAM header per stream, then
 *     frames each preceded by a SYNCPOINT when max_distance is reached.
 *   - integers use NUT's big-endian-grouped variable-length coding (v) and the
 *     zig-zag signed coding (s); fixed 64-bit startcodes are big-endian.
 *   - every startcode packet ends with a CRC-32 footer; the CRC uses the NUT
 *     polynomial 0x04C11DB7, init 0, MSB-first (non-reflected, no final xor),
 *     and is written little-endian -- matching ff_crc04C11DB7 / AV_CRC_32_IEEE.
 */

#include "replay/replay_nut.h"

#include "replay/replay_buffer.h"

#include <stdlib.h>
#include <string.h>

/* NUT startcodes: 0x4E ('N') in the top byte, so a frame_code of 'N' (reserved
 * FLAG_INVALID in the frame table) lets a demuxer resync on a startcode. */
#define NUT_MAIN_STARTCODE      (0x7A561F5F04ADULL + (((uint64_t)(('N' << 8) + 'M')) << 48))
#define NUT_STREAM_STARTCODE    (0x11405BF2F9DBULL + (((uint64_t)(('N' << 8) + 'S')) << 48))
#define NUT_SYNCPOINT_STARTCODE (0xE4ADEECA4569ULL + (((uint64_t)(('N' << 8) + 'K')) << 48))

#define NUT_ID_STRING "nut/multimedia container"  /* + an explicit NUL, 25 bytes */

#define NUT_VERSION 3
#define NUT_MAX_DISTANCE 65536u
#define NUT_MSB_PTS_SHIFT 7u  /* coded_pts >= 1<<shift => absolute pts coding */

/* Frame flags (a subset; see the NUT spec / ffmpeg nut.h). */
#define NUT_FLAG_KEY        1u
#define NUT_FLAG_CODED_PTS  8u
#define NUT_FLAG_STREAM_ID  16u
#define NUT_FLAG_SIZE_MSB   32u
#define NUT_FLAG_CHECKSUM   64u
#define NUT_FLAG_CODED      4096u

struct ReplayNutMuxer {
    FILE *out;
    ReplayNutStream *streams;
    size_t stream_count;
    uint64_t file_pos;            /* bytes written so far (we never seek) */
    uint64_t last_syncpoint_pos;  /* file offset of the previous syncpoint */
    int have_syncpoint;
};

/* ---- low-level encoders -------------------------------------------------- */

/* Unsigned variable-length value: 7 bits per byte, big-endian (most-significant
 * group first), every group but the last has the high "more data" bit set. */
static ReplayStatus nut_put_v(ReplayBuffer *b, uint64_t v)
{
    uint8_t tmp[10];
    int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7Fu);
    v >>= 7;
    while (v != 0) {
        tmp[n++] = (uint8_t)(0x80u | (v & 0x7Fu));
        v >>= 7;
    }
    while (n > 0) {
        if (replay_buffer_append_u8(b, tmp[--n]) != REPLAY_OK)
            return REPLAY_OUT_OF_MEMORY;
    }
    return REPLAY_OK;
}

/* Signed variable-length value: 0,-1,1,-2,2,... -> 0,2,1,4,3,... */
static ReplayStatus nut_put_s(ReplayBuffer *b, int64_t v)
{
    uint64_t e = (v > 0) ? (uint64_t)v * 2u - 1u : (uint64_t)(-v) * 2u;
    return nut_put_v(b, e);
}

/* CRC-32, polynomial 0x04C11DB7, init 0, MSB-first, non-reflected. */
static uint32_t nut_crc32(const uint8_t *d, size_t n)
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

static ReplayStatus nut_emit(ReplayNutMuxer *m, const void *p, size_t n)
{
    if (n != 0 && fwrite(p, 1, n, m->out) != n)
        return REPLAY_INTERNAL_ERROR;
    m->file_pos += n;
    return REPLAY_OK;
}

static ReplayStatus nut_emit_u32le(ReplayNutMuxer *m, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    return nut_emit(m, b, 4);
}

/* Emit a complete startcode packet: startcode, forward_ptr, an optional header
 * checksum (only when the packet exceeds 4096 bytes), the payload, and the CRC
 * footer. forward_ptr counts the payload plus the 4-byte footer. */
static ReplayStatus nut_emit_packet(ReplayNutMuxer *m, uint64_t startcode,
                                    const ReplayBuffer *payload)
{
    uint8_t sc[8];
    ReplayBuffer fwd;
    uint64_t fp = (uint64_t)payload->size + 4u;
    ReplayStatus st;
    int k;

    for (k = 0; k < 8; k++)
        sc[k] = (uint8_t)(startcode >> (56 - 8 * k));

    replay_buffer_init(&fwd);
    st = nut_put_v(&fwd, fp);
    if (st != REPLAY_OK)
        goto done;

    st = nut_emit(m, sc, 8);
    if (st != REPLAY_OK)
        goto done;
    st = nut_emit(m, fwd.data, fwd.size);
    if (st != REPLAY_OK)
        goto done;

    if (fp > 4096u) {
        uint8_t hbuf[8 + 10];
        memcpy(hbuf, sc, 8);
        memcpy(hbuf + 8, fwd.data, fwd.size);
        st = nut_emit_u32le(m, nut_crc32(hbuf, 8 + fwd.size));
        if (st != REPLAY_OK)
            goto done;
    }

    st = nut_emit(m, payload->data, payload->size);
    if (st != REPLAY_OK)
        goto done;
    st = nut_emit_u32le(m, nut_crc32(payload->data, payload->size));

done:
    replay_buffer_free(&fwd);
    return st;
}

/* ---- headers ------------------------------------------------------------- */

static ReplayStatus nut_write_main_header(ReplayNutMuxer *m)
{
    ReplayBuffer p;
    ReplayStatus st = REPLAY_OK;
    size_t i;

    replay_buffer_init(&p);

#define PUT_V(val) do { st = nut_put_v(&p, (val)); if (st != REPLAY_OK) goto done; } while (0)
#define PUT_S(val) do { st = nut_put_s(&p, (val)); if (st != REPLAY_OK) goto done; } while (0)

    PUT_V(NUT_VERSION);
    PUT_V(m->stream_count);
    PUT_V(NUT_MAX_DISTANCE);
    PUT_V(m->stream_count);            /* time_base_count: one per stream */
    for (i = 0; i < m->stream_count; i++) {
        PUT_V(m->streams[i].tb_num);
        PUT_V(m->streams[i].tb_den);
    }

    /* One frame-code table row covering all 256 codes with FLAG_CODED, so each
     * frame names its own flags explicitly. size_mul=1, size_lsb=0 (code 0),
     * count=255 leaves the reserved 'N' slot for the demuxer to mark invalid. */
    PUT_V(NUT_FLAG_CODED);  /* tmp_flags */
    PUT_V(6u);              /* tmp_fields: pts, mul, stream, size, res, count */
    PUT_S(0);               /* tmp_pts   */
    PUT_V(1u);              /* tmp_mul   */
    PUT_V(0u);              /* tmp_stream*/
    PUT_V(0u);              /* tmp_size  */
    PUT_V(0u);              /* tmp_res   */
    PUT_V(255u);            /* count     */

    PUT_V(0u);              /* header_count - 1: no elision headers */

    st = nut_emit_packet(m, NUT_MAIN_STARTCODE, &p);

done:
    replay_buffer_free(&p);
    return st;
#undef PUT_V
#undef PUT_S
}

static ReplayStatus nut_write_stream_header(ReplayNutMuxer *m, size_t index)
{
    const ReplayNutStream *s = &m->streams[index];
    ReplayBuffer p;
    ReplayStatus st = REPLAY_OK;
    /* ~1 second of pts units, as ffmpeg's muxer derives it. */
    unsigned max_pts_distance = s->tb_num != 0 ? s->tb_den / s->tb_num : 1u;
    if (max_pts_distance == 0)
        max_pts_distance = 1u;

    replay_buffer_init(&p);

#define PUT_V(val) do { st = nut_put_v(&p, (val)); if (st != REPLAY_OK) goto done; } while (0)

    PUT_V(index);                       /* stream_id */
    PUT_V((unsigned)s->cls);            /* stream_class: 0 video, 1 audio */
    PUT_V(4u);                          /* fourcc length */
    if (replay_buffer_append(&p, s->fourcc, 4) != REPLAY_OK) {
        st = REPLAY_OUT_OF_MEMORY;
        goto done;
    }
    PUT_V(index);                       /* time_base_id (one tb per stream) */
    PUT_V(NUT_MSB_PTS_SHIFT);
    PUT_V(max_pts_distance);
    PUT_V(0u);                          /* decode_delay */
    PUT_V(0u);                          /* stream_flags */
    PUT_V(s->extradata_len);            /* codec_specific_data */
    if (s->extradata_len != 0
        && replay_buffer_append(&p, s->extradata, s->extradata_len) != REPLAY_OK) {
        st = REPLAY_OUT_OF_MEMORY;
        goto done;
    }

    if (s->cls == REPLAY_NUT_VIDEO) {
        PUT_V(s->width);
        PUT_V(s->height);
        PUT_V(0u);                      /* sample aspect num (unknown) */
        PUT_V(0u);                      /* sample aspect den (unknown) */
        PUT_V(0u);                      /* colorspace_type */
    } else {
        PUT_V(s->sample_rate);
        PUT_V(1u);                      /* samplerate_den */
        PUT_V(s->channels);
    }

    st = nut_emit_packet(m, NUT_STREAM_STARTCODE, &p);

done:
    replay_buffer_free(&p);
    return st;
#undef PUT_V
}

/* Emit a syncpoint whose global_key_pts is the given stream's current pts. */
static ReplayStatus nut_write_syncpoint(ReplayNutMuxer *m, size_t stream_index,
                                        int64_t pts)
{
    ReplayBuffer p;
    ReplayStatus st = REPLAY_OK;
    uint64_t sp_pos = m->file_pos;  /* position of this syncpoint's startcode */
    uint64_t tt = (uint64_t)pts * m->stream_count + stream_index;
    uint64_t back = m->have_syncpoint ? (sp_pos - m->last_syncpoint_pos) >> 4 : 0;

    replay_buffer_init(&p);
    st = nut_put_v(&p, tt);            /* global_key_pts (timestamp) */
    if (st != REPLAY_OK)
        goto done;
    st = nut_put_v(&p, back);          /* back_ptr_div16 */
    if (st != REPLAY_OK)
        goto done;

    st = nut_emit_packet(m, NUT_SYNCPOINT_STARTCODE, &p);
    if (st != REPLAY_OK)
        goto done;

    m->last_syncpoint_pos = sp_pos;
    m->have_syncpoint = 1;

done:
    replay_buffer_free(&p);
    return st;
}

/* ---- public API ---------------------------------------------------------- */

ReplayNutMuxer *replay_nut_open(FILE *out, const ReplayNutStream *streams,
                                size_t stream_count, char *err, size_t errlen)
{
    ReplayNutMuxer *m;
    size_t i;

    if (out == NULL || streams == NULL || stream_count == 0) {
        if (err != NULL && errlen != 0)
            snprintf(err, errlen, "nut: invalid arguments");
        return NULL;
    }

    m = calloc(1, sizeof *m);
    if (m == NULL)
        goto oom;
    m->out = out;
    m->stream_count = stream_count;
    m->streams = malloc(stream_count * sizeof *m->streams);
    if (m->streams == NULL)
        goto oom;
    memcpy(m->streams, streams, stream_count * sizeof *m->streams);

    /* file_id_string, including its trailing NUL (sizeof covers it). */
    if (nut_emit(m, NUT_ID_STRING, sizeof NUT_ID_STRING) != REPLAY_OK)
        goto werr;
    if (nut_write_main_header(m) != REPLAY_OK)
        goto werr;
    for (i = 0; i < stream_count; i++)
        if (nut_write_stream_header(m, i) != REPLAY_OK)
            goto werr;

    return m;

oom:
    if (err != NULL && errlen != 0)
        snprintf(err, errlen, "nut: out of memory");
    goto fail;
werr:
    if (err != NULL && errlen != 0)
        snprintf(err, errlen, "nut: write error");
fail:
    if (m != NULL) {
        free(m->streams);
        free(m);
    }
    return NULL;
}

ReplayStatus replay_nut_write_frame(ReplayNutMuxer *m, size_t stream_index,
                                    int64_t pts, int keyframe,
                                    const uint8_t *data, size_t len)
{
    ReplayBuffer hdr;
    ReplayStatus st = REPLAY_OK;
    uint32_t flags = NUT_FLAG_CODED | NUT_FLAG_STREAM_ID | NUT_FLAG_CODED_PTS
                     | NUT_FLAG_SIZE_MSB | NUT_FLAG_CHECKSUM;
    uint64_t coded_pts = (uint64_t)pts + (1u << NUT_MSB_PTS_SHIFT);

    if (m == NULL || stream_index >= m->stream_count || (len != 0 && data == NULL))
        return REPLAY_INVALID_ARGUMENT;
    if (keyframe)
        flags |= NUT_FLAG_KEY;

    /* A syncpoint at the very first frame and whenever max_distance is reached;
     * raw frames are large, so in practice this is one per frame. */
    if (!m->have_syncpoint
        || m->file_pos - m->last_syncpoint_pos >= NUT_MAX_DISTANCE) {
        st = nut_write_syncpoint(m, stream_index, pts);
        if (st != REPLAY_OK)
            return st;
    }

    replay_buffer_init(&hdr);
#define PUT_V(val) do { st = nut_put_v(&hdr, (val)); if (st != REPLAY_OK) goto done; } while (0)
    if (replay_buffer_append_u8(&hdr, 0u) != REPLAY_OK) {  /* frame_code 0 */
        st = REPLAY_OUT_OF_MEMORY;
        goto done;
    }
    PUT_V(flags);            /* coded_flags (XORed onto the table's FLAG_CODED) */
    PUT_V(stream_index);     /* stream_id */
    PUT_V(coded_pts);        /* coded_pts (absolute) */
    PUT_V(len);              /* data_size_msb; size = msb*1 + 0 */
#undef PUT_V

    /* The frame checksum protects the header so a corrupt size can't run away;
     * NUT requires it once a frame exceeds max_distance. */
    st = nut_emit(m, hdr.data, hdr.size);
    if (st != REPLAY_OK)
        goto done;
    st = nut_emit_u32le(m, nut_crc32(hdr.data, hdr.size));
    if (st != REPLAY_OK)
        goto done;
    st = nut_emit(m, data, len);

done:
    replay_buffer_free(&hdr);
    return st;
}

ReplayStatus replay_nut_close(ReplayNutMuxer *m)
{
    if (m == NULL)
        return REPLAY_OK;
    if (fflush(m->out) != 0) {
        free(m->streams);
        free(m);
        return REPLAY_INTERNAL_ERROR;
    }
    free(m->streams);
    free(m);
    return REPLAY_OK;
}
