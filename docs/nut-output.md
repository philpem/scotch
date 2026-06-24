# NUT container output

`replay-transcode --output-format nut` muxes the decoded video and sound into a
single [NUT](https://ffmpeg.org/nut.html) stream instead of emitting headerless
RGB24 on stdout plus a sidecar WAV. NUT is the low-overhead container designed
by the FFmpeg/MPlayer developers; ffmpeg demuxes it directly from a pipe, so the
whole transcode becomes:

```sh
build/replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
    --output-format nut | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

Compared with raw mode this removes three sources of friction:

- **No manual geometry.** The width, height, frame rate, pixel format, sample
  rate and channel count travel in the container header, so ffmpeg no longer
  needs `-f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS`.
- **One stream, no sidecar.** Audio and video are interleaved in the same pipe;
  there is no temporary WAV file and no second `-i` input.
- **No stderr command to copy.** Raw mode prints the exact ffmpeg invocation to
  stderr because the user otherwise can't know the geometry; nut mode just needs
  `ffmpeg -i -`.

Raw mode (the default) is unchanged and still useful when you want the bare
RGB24 frames or a standalone WAV.

## What the stream contains

- A **video stream** of rawvideo `rgb24` (NUT codec tag `RGB\x18`), one frame
  per decoded Replay frame, each marked as a keyframe (every frame is a full
  RGB24 image after decoding). Its time base is `1/fps`.
- An **audio stream** of `pcm_s16le` (tag `PSD\x10`) when the movie has a sound
  track this tool can decode, time base `1/sample_rate`. Channels and rate come
  from the movie header. A sound-only movie produces an audio-only NUT stream.

Audio is interleaved per chunk: for each Replay chunk the chunk's decoded PCM is
emitted just before that chunk's video frames. (The native type-23 path is not
chunk-iterated, so there the per-chunk audio packets are emitted up front; the
result is the same single muxed stream.)

`--audio-output` is meaningless in nut mode (the sound is muxed) and is ignored
with a warning. `--audio-format`, `--video-colour`, `--module(s-dir)` and
`--skip-unsupported` behave exactly as in raw mode.

## Implementation notes

The muxer lives in `src/replay_nut.c` (`include/replay/replay_nut.h`) and is
deliberately codec-agnostic: each stream carries a NUT codec tag and optional
extradata, so besides the rawvideo/PCM streams used here it can mux
already-compressed packets. That is the intended foundation for transcoding
re-encapsulated formats (e.g. the MovieFS codecs) straight into NUT and letting
ffmpeg decode them.

The bitstream mirrors ffmpeg's own `libavformat/nutenc.c` field for field:

- `file_id_string`, a MAIN header (version 3), one STREAM header per stream,
  then frames each preceded by a SYNCPOINT once `max_distance` bytes have
  elapsed.
- Integers use NUT's big-endian-grouped variable-length coding; 64-bit
  startcodes are big-endian.
- Every startcode packet ends with a CRC-32 footer and every frame carries an
  absolute pts and a header CRC (NUT polynomial `0x04C11DB7`, init 0, MSB-first,
  written little-endian — the `AV_CRC_32_IEEE` variant ffmpeg uses).
- A single frame-code table row gives all 256 codes `FLAG_CODED`, so each frame
  states its own flags explicitly; output is purely sequential (no index is
  written), which is ideal for a pipe.

### Testing

- `test_nut_mux` (always runs) re-derives the CRC and varint coding
  independently and checks the file id string, startcodes and packet framing.
- `test_transcode_nut` (gated on an available `ffmpeg`) is the end-to-end oracle:
  it transcodes a synthetic movie both ways and confirms ffmpeg demuxes the NUT
  stream back to the exact same RGB24 video as raw mode and the exact same PCM as
  the source.
