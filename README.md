# Scotch

_"Re-record not fade away"_

Scotch is a reference implementation of the Acorn Replay video compression
scheme, as developed by Sophie Wilson et al at Acorn in the 1990s. It can:

  * Accurately document the Replay bitstreams and compression methodology.
  * Encode video from modern sources to Replay (using ffmpeg as an input adapter).
  * Transcode Replay movies back to video, decoding each frame with the original
    Acorn `Decomp` codec modules run under a bundled ARMulator.

## History

Scotch started out as a coder and multiplexer for Acorn's Super Moving Blocks
codec, the latest "released" Moving Blocks variant and the second-to-last
evolution of it. Structurally it has most or all of the elements of the earlier
Moving Blocks schemes, so implementing it gave a solid base for the rest.

Development began with a reimplementation of the Super Moving Blocks decoder,
cross-checked against the original Acorn decoder (written in ARM assembler). That
gave the LLM (Claude) a firm pass/fail test result, which in turn drove the
encoder. Once Super Moving Blocks was complete, the other Moving Blocks coders —
and Moving Lines — were added, including the September and November 1996 variants
of Moving Blocks Beta.

## Documentation

- [docs/implementation-status.md](docs/implementation-status.md) — implemented
  surface, verified claims, and known differences from the original compressors.
- [docs/encoder-policy-comparison.md](docs/encoder-policy-comparison.md) — how
  decision-policy differences affect bitrate and quality.
- [docs/spec/](docs/spec/) — implementation-grade format specifications with
  provenance appendices (start with
  [docs/spec/methodology.md](docs/spec/methodology.md)).

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Builds default to `Release` (`-O3`) when no build type is given; the encoder
(rate control + motion search) is roughly 5x slower without it. Use
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` for an unoptimised, debuggable
build.

The C implementation has no third-party runtime dependency. The optional ARM
cross-check tests are self-contained and use a vendored ARMulator core derived
from the GDB sources (see [vendor/armulator](vendor/armulator)).

## Tools

| Tool | Purpose |
|------|---------|
| `replay-inspect` | Inspect an ARMovie/AE7 header and its validated chunk catalogue (labels codecs by number and name, e.g. "type 19, Super Moving Blocks"). |
| `replay-extract` | Pull fixed-size raw frames out of uncompressed intermediates (types 2 and 23). |
| `replay-encode` | Encode RGB24 (or native packed) frames to raw codec payloads. |
| `replay-join` | Assemble payloads, sound, and a poster into a playable `.ae7` movie. |
| `replay-transcode` | Decode a Replay movie back to video for ffmpeg: raw RGB24 (+ WAV sound), or a muxed NUT container (`--output-format nut`). |
| `replay-verify` | Decode/verify raw payloads and cross-check against Acorn output. |
| `tools/replay-make` | One-command ffmpeg → encode → join pipeline. |

## Quick start: make a movie

```sh
tools/replay-make input.mp4 movie,ae7 --width 320 --loss-level 8
```

`replay-make` wraps ffmpeg, `replay-encode`, and `replay-join` with temporary
intermediates. It picks an aspect-correct height (a multiple of 4), encodes type
19 video, adds an 11025 Hz mono VIDC-E8 sound track, and embeds a poster (the
first frame, or `--poster IMAGE`; `--no-poster` to skip). Other options: `--fps`,
`--frames-per-chunk`, `--start`/`--duration`, `--audio-rate`, `--no-audio`.

## Encode

Encode every complete frame of an RGB24 stream as separate raw payloads:

```sh
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size 320x256 \
    --payload-prefix frames/frame- --loss-level 7 --target-bytes 4096 \
    --trace frames/decisions.txt
```

The encoder converts RGB with CompLib's non-dithered fixed-point path, decodes
every generated payload, and compares the result against its reconstructed frame
before writing. After the first (key) frame, accepted reconstructions use
stationary, temporal, spatial, and split block modes. For the full option set —
single-frame mode, `--policy`, `--target-bytes` rate control, native `6y5uv`
input, quality sweeps, and the production ffmpeg pipeline (frame rate, scale/pad,
exact sizing, error propagation) — see
[docs/ffmpeg-input.md](docs/ffmpeg-input.md) and
[notes/rate-control.md](notes/rate-control.md). Verifier-based comparison against
an Acorn reference is covered in
[docs/acorn-comparison-workflow.md](docs/acorn-comparison-workflow.md).

## Join

The individual steps mirror Acorn's own `Join` tool (compress, then join):

```sh
build/replay-join --codec 19 --size 160x128 --fps 12.5 \
    --frames-prefix frames/frame- --frames 25 --pixel-label 6Y5UV \
    --chunk-seconds 1.0 --output movie,ae7
```

The writer reproduces Join's layout: a 21-line header, a sector-aligned chunk
catalogue, even/odd double-buffer sizes, and chunks holding even-padded video
followed by interleaved sound. A poster sprite is embedded by default (the
`!ARPlayer` GUI crashes without one); use `--poster FILE.bgr555` for a custom
16bpp poster or `--no-poster` for command-line-player-only output. Audio can be
pre-encoded or supplied as PCM that the tool encodes itself (`--sound-pcm` /
`--sound-encode`). See
[notes/replay-ae7-join-writer.md](notes/replay-ae7-join-writer.md) for chunking,
alignment, and the sound formats.

## Transcode back to video

`replay-transcode` parses a movie, decodes each frame, and either streams RGB24
to stdout (optionally writing a sidecar WAV) or muxes audio and video together
into a NUT container that pipes straight into ffmpeg.

The easy path is `--output-format nut`: one self-describing stream carries the
video, the sound, and the geometry/frame-rate, so ffmpeg needs no `-f rawvideo
-video_size … -framerate …` and no second input:

```sh
build/replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
    --output-format nut | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

The default raw mode streams headerless RGB24 to stdout and writes the sound to
a separate WAV; it needs the geometry and rate spelled out to ffmpeg:

```sh
build/replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
    --audio-output sound.wav \
  | ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS \
      -i - -i sound.wav -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

In raw mode the exact ffmpeg command (with this movie's geometry, rate, and any
sound) is printed to stderr, so you can copy it. See
[docs/nut-output.md](docs/nut-output.md) for the container details. The compiled
decompressor is not stored
in the movie, so for the codecs that need one supply it with `--module FILE` or
`--modules-dir DIR` (from which `DecompN/Decompress,ffd` is taken). The codec
modules are vendored in
[vendor/armovie-codecs](vendor/armovie-codecs/README.md), which documents each
codec and its colour model. Use `--video-colour` to override the colour model
(needed for Escape, which declares none), `--audio-format` to pick the sound
encoding, and `--skip-unsupported` for partial (video- or audio-only) output.
A sound-only movie (video format 0) is transcoded to audio only; it has no
codec module, so `--modules-dir` is ignored for it even if some such movies
carry stray non-zero dimensions in the header.

MovieFS (Warm Silence Software) re-encapsulated PC codecs — video formats
600–699, e.g. Cinepak (602) — are supported by driving their compiled MovieFS
decompressor: the tool strips MovieFS's 16-byte per-frame wrapper and selects a
colour-table-free variant so the module runs unpatched. The WSS freeware modules
are vendored, so the default `--modules-dir vendor/armovie-codecs` works:

```sh
build/replay-transcode --input ironman.rpl --modules-dir vendor/armovie-codecs \
    --output-format nut | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

Native (vendored module): 602 Cinepak (validated), 608/626 RGB24, 615 QT-RLE24
(`Dec24`→RGB); the palettised `Dec8` family 600 CRAM8 / 604 SMC / 606·624 RGB8 /
607·609 RLE8 / 613 RLE4 / 622 DL / 623 ANM (8-bit indices coloured via the
movie's `palette <offset>`); and 614 QT-RLE16 (`Decompress`→RGB555).

Codecs better handled by ffmpeg use **codec pass-through**: with `--output-format
nut` the frames are de-wrapped and muxed under a codec fourcc so ffmpeg decodes
them. Wired: 601 CRAM16, 603 RPZA, 605 Ultimotion (MovieFS), and the Indeo codecs
628/629 (MovieFS) and 901/902 (IMS VideoFS: 901 raw YVU9, 902 Indeo 3.2).

Apart from 602 Cinepak (validated end-to-end), the 6xx/9xx mappings are derived
from the codec sources and not yet validated against real movies. See
[docs/moviefs-nut-passthrough.md](docs/moviefs-nut-passthrough.md) for the codec
inventory, the variant analysis, and the VideoFS framing.

Type 500, Iota's "The Complete Animator" (TCA/ACEF), is decoded natively by
`replay_tca` (no module, no ffmpeg) — the film is embedded in the Replay
container and decoded to 8bpp + its palette. 8-bit modes (28/21) work end to end
(`--skip-unsupported` skips the Iota sound track); the 4-bit modes are future
work. See [docs/spec/tca-type500.md](docs/spec/tca-type500.md).

## Acorn cross-check

An optional CTest decodes the checked-in format-19 corpus with the original
compiled Acorn `Decompress,ffd` (vendored under
[vendor/armovie-codecs](vendor/armovie-codecs/README.md)) and compares its packed
`6Y5UV` output byte-for-byte with the portable verifier. The native harness
(`replay-armsim`) runs the module under the vendored ARMulator and needs no
third-party dependency (`test_armsim_corpus`); the older Python/Unicorn harnesses
remain for additional cross-checks when Python and the Unicorn bindings are
installed. Details, including the ARM memory semantics that remove the need for
alignment shims, are in
[docs/decomp19-arm-harness.md](docs/decomp19-arm-harness.md). The corpus also
contains two frames made by the original Acorn Super Moving Blocks compressor,
including one temporal dependency.

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names: `codec_movingblocks`,
  `codec_movingblockshq`, `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.
