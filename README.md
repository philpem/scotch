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
| `replay-transcode` | Decode a Replay movie back to video for ffmpeg: a muxed NUT stream (default), or split RGB24 + WAV sidecar (`--output-format raw`). |
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

`replay-transcode` parses a movie, decodes each frame, and **by default** muxes
the video, sound and geometry/frame-rate into a single NUT stream that pipes
straight into ffmpeg — no `-f rawvideo -video_size … -framerate …`, no second
input:

```sh
build/replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
  | ffmpeg -i - -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
```

`--output-format raw` selects the older split form: headerless RGB24 frames to
stdout plus an optional `--audio-output sound.wav` sidecar. It needs the geometry
and rate spelled out to ffmpeg (the exact command is printed to stderr), and it
**cannot** carry the pass-through codecs (Indeo, CRAM16, …) that only ffmpeg can
decode — those require the default NUT output:

```sh
build/replay-transcode --input movie,ae7 --modules-dir vendor/armovie-codecs \
    --output-format raw --audio-output sound.wav \
  | ffmpeg -f rawvideo -pixel_format rgb24 -video_size WxH -framerate FPS \
      -i - -i sound.wav -c:v libx264 -pix_fmt yuv420p -c:a aac out.mp4
``` See
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

Native (vendored module): 602 Cinepak and 600 CRAM8 (both validated), 608/626
RGB24, 615 QT-RLE24 (`Dec24`→RGB); the rest of the palettised `Dec8` family 604
SMC / 606·624 RGB8 / 607·609 RLE8 / 613 RLE4 / 622 DL / 623 ANM (8-bit indices
coloured via the movie's `palette <offset>`); and 614 QT-RLE16 (`Decompress`→
RGB555). CRAM8 carries its own per-chunk palette inline (a `0xffffffff` marker +
256 RGB555 entries before the wrapper chain) and is stored bottom-up. MovieFS
movies are decoded at the true frame size from the per-frame wrapper (the AE7
header rounds the width up). See
[docs/moviefs-nut-passthrough.md](docs/moviefs-nut-passthrough.md).

Codecs better handled by ffmpeg use **codec pass-through**: with `--output-format
nut` the frames are de-wrapped and muxed under a codec fourcc so ffmpeg decodes
them. Wired: 601 CRAM16, 603 RPZA, 605 Ultimotion, 610 FLI/FLC (MovieFS), the
Indeo codecs 628/629 (MovieFS) and 901/902 (IMS VideoFS: 901 raw YVU9, 902 Indeo
3.2), and **130 Eidos "Escape 2.0"** (the games-era sibling ffmpeg decodes as
`escape130`, fourcc `E130`; validated end-to-end on seven real samples). FLIC's
decode path is validated with a synthesised frame (ffmpeg tracks its in-stream
palette); the remaining MovieFS/VideoFS mappings await real samples.

**Escape 122** is decoded natively (`replay_esc122`): it is a *palettised* (PAL8)
codec — unrelated to escape124/130, and not decodable by the Eidos Streamer DLLs —
so `tank.rpl` transcodes (video + sound). **Escape 124** (a games-era RGB555 block
codec, `WINSDEC`/`EDEC`) is also decoded natively (`replay_esc124`), walking the
several frames each chunk packs; validated byte-for-byte against both a reference
decoder and ffmpeg's own `escape124` on `ESCAPE.RPL`/`PYRAMID.RPL`. Escape 100/102
(ARM modules) are not yet wired. See
[docs/spec/eidos-escape.md](docs/spec/eidos-escape.md).

Apart from 602 Cinepak (validated end-to-end), the 6xx/9xx mappings are derived
from the codec sources and not yet validated against real movies. See
[docs/moviefs-nut-passthrough.md](docs/moviefs-nut-passthrough.md) for the codec
inventory, the variant analysis, and the VideoFS framing.

Type 500, Iota's "The Complete Animator" (TCA/ACEF), is decoded natively by
`replay_tca` (no module, no ffmpeg) — the film is embedded in the Replay
container and decoded to 8bpp + its palette for the numbered screen modes (8-bit
28/21/15/36/40 and 4-bit 27/12/13/39), and to RGB for new-format mode words
including 16bpp direct colour (RGB555). The Iota soundtrack (`SOUN` WAV1/WAV2) is decoded to mono
PCM and muxed too, so type-500 movies transcode with sound. See
[docs/spec/tca-type500.md](docs/spec/tca-type500.md).

Type 800, **LinePack** (Henrik Bjerregaard Pedersen), is decoded by its vendored
`Decomp800` module. It decodes at the exact declared frame size — its Info "step"
is an alignment hint, not a padding requirement, so the transcoder skips block
rounding for it (`exact_size`). Validated end-to-end on a real 160×120 sample. See
[docs/spec/linepack-type800.md](docs/spec/linepack-type800.md).

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
