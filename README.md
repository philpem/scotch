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
