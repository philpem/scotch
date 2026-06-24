# FFmpeg Input

The portable encoder accepts a headerless stream of fixed-size RGB24 frames.
FFmpeg performs container demuxing, modern codec decoding, frame-rate
conversion, scaling, and pixel conversion before writing that stream.

## Recommended Pipeline

Use a shell with pipeline failure propagation so an FFmpeg decoding error is
not hidden by the encoder successfully consuming an early EOF:

```sh
set -o pipefail
mkdir -p frames
ffmpeg -v error -i input.mkv -an \
    -vf "fps=12.5,scale=320:256:force_original_aspect_ratio=decrease,pad=320:256:(ow-iw)/2:(oh-ih)/2" \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --input-format rgb24 \
        --size 320x256 --payload-prefix frames/frame- --loss-level 7 \
        --target-bytes 4096 --trace frames/encode.trace
```

`--size` must exactly match FFmpeg's final output dimensions. RGB24 contains
three bytes per pixel with no row padding, so one 320x256 input frame is
245,760 bytes. The encoder rejects a partial final frame. `--frames N` also
requires exactly N complete frames and then EOF. Use FFmpeg's `-frames:v N` to
truncate a longer source before it reaches the encoder.

The example preserves display aspect ratio and pads with black. Replace the
filter with `scale=320:256` only when stretching is intentional. Type 19,
Super Moving Blocks currently requires dimensions that are multiples of four.

## Colour Conversion

FFmpeg outputs RGB24 because it is unambiguous and widely supported. The C
encoder then applies the documented CompLib-style RGB-to-6Y5UV conversion.
This keeps Replay-specific quantisation in one testable implementation.

FFmpeg pixel formats such as `yuyv422` are not accepted directly yet. Although
Replay type 21 is also named YUYV8 and type 23 is packed 4:2:2, their packing,
precision, range, and signed-chroma conventions must be verified before
claiming direct byte compatibility.

## Library Integration

Direct libavformat/libavcodec integration remains optional future work. A raw
pipe exposes FFmpeg's full decoder and filter graph without making the codec
library depend on FFmpeg ABI versions. Library integration becomes worthwhile
if the final CLI needs seeking, embedded audio, timestamp-driven selection, or
better cross-platform process error reporting.

On the *output* side (`replay-transcode`), muxed single-pipe output is already
available without any libav dependency: `--output-format nut` writes a NUT
container that carries the video, the sound, and the geometry/frame-rate in one
self-describing stream. See [nut-output.md](nut-output.md). The hand-written
muxer keeps the no-third-party-runtime-dependency guarantee intact.

CTest exercises the pipe with FFmpeg's generated `testsrc2` source when an
`ffmpeg` executable is available.
