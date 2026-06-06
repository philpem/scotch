#ifndef CODEC_SUPERMOVINGBLOCKS_H
#define CODEC_SUPERMOVINGBLOCKS_H

#include "replay/mb_codec.h"
#include "replay/mb_frame.h"

extern const MbHuffmanTable codec_supermovingblocks_luma_huffman;

/*
 * These switches enable alternatives to a 4x4 data block. At loss level 0 a
 * copy is used only when it reconstructs exactly the same quantised pixels as
 * the corresponding data representation. Levels 1 through 28 use the
 * original compressor's progressively looser acceptance table.
 *
 * Stationary and temporal modes require `previous`. Spatial mode only refers
 * to pixels already reconstructed in the current frame, so it is also legal
 * in a key frame. Split considers the corresponding data, stationary,
 * temporal, and spatial 2x2 modes.
 */
typedef struct {
    int allow_stationary;
    int allow_temporal;
    int allow_spatial;
    int allow_split;
    /* 0 is exact; 28 is the loosest source-defined QP% row. */
    unsigned loss_level;
} CodecSuperMovingBlocksEncodeOptions;

/* Counts describe the selected stream, not the candidates considered. */
typedef struct {
    size_t data4x4_blocks;
    size_t stationary4x4_blocks;
    size_t temporal4x4_blocks;
    size_t spatial4x4_blocks;
    size_t split4x4_blocks;
    size_t data2x2_blocks;
    size_t stationary2x2_blocks;
    size_t temporal2x2_blocks;
    size_t spatial2x2_blocks;
    size_t bits_written;
} CodecSuperMovingBlocksEncodeStats;

typedef enum {
    CODEC_SUPERMOVINGBLOCKS_MODE_DATA,
    CODEC_SUPERMOVINGBLOCKS_MODE_STATIONARY,
    CODEC_SUPERMOVINGBLOCKS_MODE_TEMPORAL,
    CODEC_SUPERMOVINGBLOCKS_MODE_SPATIAL,
    CODEC_SUPERMOVINGBLOCKS_MODE_SPLIT
} CodecSuperMovingBlocksMode;

/* One successfully decoded block or split container. */
typedef struct {
    unsigned x;
    unsigned y;
    unsigned size;
    CodecSuperMovingBlocksMode mode;
    /* Stream range [bit_start, bit_end), including this block's opcode. */
    size_t bit_start;
    size_t bit_end;
    /* Meaningful only for temporal and spatial events. */
    int motion_dx;
    int motion_dy;
} CodecSuperMovingBlocksDecodeEvent;

typedef void (*CodecSuperMovingBlocksDecodeTrace)(
    const CodecSuperMovingBlocksDecodeEvent *event, void *opaque);

ReplayStatus codec_supermovingblocks_decode_data4x4(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_supermovingblocks_decode_data2x2(
    ReplayBitReader *reader, MbPredictor *predictor,
    MbPixel *pixels, size_t stride, MbVerifyError *error);

ReplayStatus codec_supermovingblocks_write_data4x4(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[16]);

ReplayStatus codec_supermovingblocks_write_data2x2(
    ReplayBitWriter *writer, uint8_t u, uint8_t v,
    const uint8_t residuals[4]);

/*
 * Encode a complete frame using only 4x4 data blocks. The reconstructed frame
 * receives the exact pixels a decoder will produce, including block-averaged
 * chroma. Output is replaced on entry and contains one byte-padded payload.
 */
ReplayStatus codec_supermovingblocks_encode_data_frame(
    const MbFrame *source, ReplayBuffer *output, MbFrame *reconstructed,
    size_t *bits_written);

/*
 * Encode one frame using the enabled block decisions and selected loss level.
 *
 * `source` contains quantised 6Y5UV samples. `reconstructed` is filled with
 * exactly what a format-19 decoder will retain and must therefore be supplied
 * as `previous` for the next inter frame. `output` is cleared even when source
 * validation fails, preserving replacement semantics.
 */
ReplayStatus codec_supermovingblocks_encode_frame(
    const MbFrame *source, const MbFrame *previous,
    const CodecSuperMovingBlocksEncodeOptions *options, ReplayBuffer *output,
    MbFrame *reconstructed, CodecSuperMovingBlocksEncodeStats *stats);

ReplayStatus codec_supermovingblocks_verify_frame(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error);

/*
 * Decode as above and report every selected mode. Split children are reported
 * first, followed by the enclosing split event and its complete bit range.
 */
ReplayStatus codec_supermovingblocks_verify_frame_traced(
    const uint8_t *payload, size_t payload_size,
    const MbFrame *previous, MbFrame *decoded,
    size_t *bits_consumed, MbVerifyError *error,
    CodecSuperMovingBlocksDecodeTrace trace, void *trace_opaque);

#endif
