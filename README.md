# Replay Tooling

Portable C tooling for inspecting, verifying, and encoding Acorn Replay video
streams.

The implemented target is compression type 19, Super Moving Blocks. The project
currently provides byte/bitstream primitives, Moving Blocks codec descriptors,
a complete type 19, Super Moving Blocks payload verifier, a deterministic
encoder with the
original 29-level copy-match table, frame-level rate retries, and automated
cross-checks against Acorn's compiled ARM decompressor.

See [docs/implementation-status.md](docs/implementation-status.md) for the
implemented surface, verified claims, and known differences from the original
compressor. [docs/encoder-policy-comparison.md](docs/encoder-policy-comparison.md)
explains how decision-policy differences can affect bitrate and quality.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Builds default to `Release` (`-O3`) when no build type is given; the encoder
(rate control + motion search) is roughly 5x slower without it. Use
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` if you need an unoptimised,
debuggable build.

The C implementation has no third-party runtime dependency. The optional ARM
cross-check tests require Python 3 with Unicorn bindings. CMake automatically
uses `../!ARMovie_compiled/Decomp19/Decompress,ffd` when present; another copy
can be selected with `-DDECOMP19_COMPILED=/path/to/Decompress,ffd`.

Inspect an ARMovie/AE7 header and its validated chunk catalogue with:

```sh
build/replay-inspect ../LionFish19,ae7
```

The inspector labels known compression identifiers by both number and name,
for example type 19, Super Moving Blocks. In the AE7 header, `number of chunks`
is the last zero-based chunk index, so the tool reports both that value and the
derived catalogue-entry count.

Check the type 19, Super Moving Blocks Huffman table with:

```sh
build/replay-verify --codec 19 --verify-huffman
```

Verify a raw type 19, Super Moving Blocks frame payload with no temporal
dependencies:

```sh
build/replay-verify --codec 19 --payload frame.mb19 --size 320x256
```

Add `--summary` for machine-readable selected-mode counts, per-mode bit totals,
motion extent, semantic bits, and stored payload bytes. Aggregate multiple
verifier reports without incorrectly averaging frame PSNR values with:

```sh
tools/mb19_compare_reports.py acorn=acorn.report portable=portable.report
```

Export decoded packed `Y,U,V` bytes and compare them with an Acorn reference:

```sh
build/replay-verify --codec 19 --payload frame.mb19 --size 320x256 \
    --previous-6y5uv previous.6y5uv --output-6y5uv decoded.6y5uv \
    --expect-6y5uv acorn-decoded.6y5uv
```

The payload verifier currently accepts all type 19, Super Moving Blocks block
modes. A payload
containing temporal references requires the library API so the caller can
supply the previous reconstructed frame, or the CLI's `--previous-6y5uv`
option. The CLI rejects temporal payloads when no reference is supplied rather
than inventing pixels. The original-codec corpus contract is documented in
`corpus/format19/README.md`.

Encode exactly one packed RGB24 frame as a raw type 19, Super Moving Blocks
payload:

```sh
ffmpeg -i input.mp4 -vf scale=320:256 -frames:v 1 \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size 320x256 \
    --payload frame.mb19 --trace frame.trace --recon-ppm frame.ppm
```

The encoder converts RGB with CompLib's non-dithered fixed-point path, decodes
every generated payload, and compares the result with its reconstructed frame
before writing the payload. A first/key frame cannot use temporal modes, but it
may use spatial and split modes unless `--data-only` is supplied. Single-frame
mode requires EOF after that frame.

See [docs/ffmpeg-input.md](docs/ffmpeg-input.md) for a production-oriented
pipeline covering frame rate, aspect-ratio-preserving scale/pad, exact frame
sizing, partial input, and pipeline error propagation.

For comparison inputs already expressed in the codec's native packed byte
triplets, use `--input-format 6y5uv`. The default remains `rgb24`; native input
is range-checked and avoids an unnecessary RGB conversion.

Encode all complete frames from an RGB24 stream as separate raw payloads:

```sh
ffmpeg -i input.mp4 -vf scale=320:256,fps=12.5 \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size 320x256 \
    --payload-prefix frames/frame- --loss-level 7 --target-bytes 4096 \
    --trace frames/decisions.txt
```

This writes `frame-000000.mb19`, `frame-000001.mb19`, and so on. After the
first frame, accepted same-position reconstructions use two-bit stationary
blocks, and accepted matches elsewhere use temporal motion codes.
Spatial copies may reference pixels already reconstructed in the same
frame, including in key frames. Split blocks are selected by emitted bit cost
when a mixture of 2x2 data, stationary, temporal, and spatial blocks is smaller
than 4x4 data.
`--loss-level N` selects source-defined level 0 through 28; level 0 requires
exact copy matches. `--data-only` disables copy and split decisions, and
`--frames N` requires exactly N input frames. The output files remain raw
codec payloads rather than an undocumented temporary container.

The default `--policy lowest-error` compares every accepted stationary,
temporal, and spatial copy by decoder-visible error, then emitted bits, then a
stable family/table order. `--policy ordered` preserves the earlier portable
stationary-then-temporal-then-spatial behavior for regression comparisons.

`--target-bytes N` enables Acorn-style whole-frame retries after the first
frame. The accepted size window is 90% through 102.5% of the target. The
encoder moves one loss level per retry, keeps the initial direction to avoid
oscillation, and carries the accepted level into the next frame. Trace output
records every verifier-clean attempt, including rejected retries. Library
callers can override both floating-point window factors through
`mb_rate_control_init_window`; calculated byte limits are explicitly truncated.

Encoder traces include native 6Y5UV error metrics for every attempt. The
verifier can produce a per-block trace and compare decoded output with a source
frame using `--reference-6y5uv`. See
[docs/acorn-comparison-workflow.md](docs/acorn-comparison-workflow.md).

Run a reproducible fixed-level policy sweep over numbered native frames with:

```sh
python3 tools/mb19_quality_sweep.py \
    --encode build/replay-encode --verify build/replay-verify \
    --source-prefix source/frame- --frames 25 --size 160x128 \
    --levels 0,7,14,21,28 --policies lowest-error ordered \
    --work-dir sweep --output sweep.md
```

Each run retains payloads, reconstructed native frames, encoder traces, and
verifier reports. The Markdown table uses sequence PSNR calculated from summed
squared error, not an average of per-frame PSNR values.

The same driver accepts `--targets 5000,6000,7000 --initial-level 7` instead
of `--levels` for matched target-byte experiments. It uses
`--rate-search bracketed`: try the adjacent quality row, expand exponentially
until the target is crossed, then refine the bracket. Direct CLI users may
select `linear` to preserve Acorn-style adjacent-row retries. Every probe is
still independently decoded and checked. Use a release build for corpus
sweeps; exhaustive motion search is intentionally slow in an `-O0` build.
The CLI keeps an explicit per-frame temporal-search workspace across retries:
the first search measures each candidate once and records its result for all
29 quality rows. Spatial searches are deliberately not cached because their
reference pixels depend on the reconstruction produced by that retry.
The five-frame 6,000-byte regression remains byte-identical and performs 53.8%
fewer temporal pixel comparisons with the cache enabled.

Extract fixed-size frames from a type 2 (16 bit colour uncompressed) Replay
intermediate with:

```sh
mkdir -p source
build/replay-extract --input movie,ae7 --output-prefix source/frame- \
    --type2-layout type19-fields
```

`type19-fields` explicitly reinterprets each stored halfword as the fields the
historical type 19 (Super Moving Blocks) compressor saw. It does not claim to
convert the type 2 movie's RGB555, YUV555, or 6Y5UV colour space. The option is
mandatory so this potentially surprising interpretation cannot happen
silently.

Type 23 (`6Y6Y5U5V`) packed 4:2:2 can be extracted with:

```sh
build/replay-extract --input movie,ae7 --output-prefix source/frame- \
    --type23-layout 6y6y5u5v
```

Each horizontal pair has two six-bit luma samples and one shared five-bit U/V
pair, with no vertical chroma subsampling. Extracted output expands the shared
chroma to both pixels as packed `Y,U,V` triplets.

Build a complete, playable movie from any video file in one command with the
`tools/replay-make` driver (it wraps ffmpeg, `replay-encode` and `replay-join`,
using temporary intermediate files):

```sh
tools/replay-make input.mp4 movie,ae7 --width 320 --loss-level 8
```

It picks an aspect-correct height (a multiple of 4), encodes type 19 video,
adds an 11025 Hz mono VIDC-E8 sound track, and embeds a poster (the first frame,
or `--poster IMAGE`; `--no-poster` to skip). Other options: `--fps`,
`--frames-per-chunk`, `--start`/`--duration`, `--audio-rate`, `--no-audio`.

The individual steps, if you need them, mirror Acorn's own `Join` tool
(compress, then join):

```sh
ffmpeg -i input.mp4 -vf scale=160:128,fps=12.5 -pix_fmt rgb24 -f rawvideo - | \
  build/replay-encode --codec 19 --input - --size 160x128 \
    --payload-prefix frames/frame- --loss-level 7
build/replay-join --codec 19 --size 160x128 --fps 12.5 \
    --frames-prefix frames/frame- --frames 25 --pixel-label 6Y5UV \
    --chunk-seconds 1.0 --output movie,ae7
```

The three encoder rate-control modes -- fixed quality (`--loss-level`), fixed
frame size (`--target-bytes`), and device bandwidth (`replay-make
--data-rate KB [--latency S] [--double]`) -- are described in
[notes/rate-control.md](notes/rate-control.md).

`replay-join` embeds a poster sprite (required by the !ARPlayer GUI, which
crashes without one): `--poster FILE.bgr555` for a custom 16bpp poster, the
built-in Replay logo by default, or `--no-poster` for command-line-player-only
output.

The writer reproduces Join's layout: a 21-line header, a sector-aligned chunk
catalogue, even/odd double-buffer sizes, and chunks holding even-padded video
followed by interleaved sound. `--frames-per-chunk N` fixes the count; otherwise
`--chunk-seconds S` targets a duration and distributes frames across chunks
(fractional rates such as 12.5 fps alternate 12/13 frames per chunk, and the
header records the fractional `12.5 frames per chunk` faithfully). `--align MASK`
sets the sector mask (default 2047 = 2048-byte alignment). Audio is added either pre-encoded
(`--sound FILE --sound-format vidc-log|<name>`) or, more usefully, as canonical
signed-16 little-endian PCM from ffmpeg that the tool encodes itself:

```sh
ffmpeg -i input.mp4 -vn -ac 1 -ar 22050 -f s16le audio.pcm
build/replay-join --codec 19 --size 160x96 --fps 12.5 \
    --frames-prefix frame- --frames 100 --pixel-label 6Y5UV \
    --sound-pcm audio.pcm --sound-encode vidc-e8 \
    --sound-rate 22050 --sound-channels 1 --output movie.ae7
```

`--sound-encode vidc-e8` uses the 8-bit VIDC exponential companding (the
nearest-match inverse of Acorn's `ELogToLinTable`); `signed-8` and `signed-16`
are the linear PCM formats. The track is time-sliced per chunk so audio stays
aligned with video. See
[notes/replay-ae7-join-writer.md](notes/replay-ae7-join-writer.md). Player
playback is not yet verified, but the output round-trips through the reader and
the compiled Decomp19.

## Acorn Cross-Check

When `../!ARMovie_compiled/Decomp19/Decompress,ffd` and Python Unicorn bindings
are present, CTest runs the compiled Acorn decompressor against generated data
and stationary frames and compares its packed `6Y5UV` output byte-for-byte with
the portable verifier. Details, including the classic ARM alignment shim, are
in [docs/decomp19-arm-harness.md](docs/decomp19-arm-harness.md).
The checked-in corpus also contains two frames made by the original Acorn type
19, Super Moving Blocks compressor, including one temporal dependency.

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names:
  `codec_movingblocks`, `codec_movingblockshq`,
  `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.
