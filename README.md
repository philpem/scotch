# Replay Tooling

Portable C tooling for inspecting, verifying, and encoding Acorn Replay video
streams.

The first implemented target is compression type 19, Super Moving Blocks. The
current milestone provides byte/bitstream primitives, Moving Blocks codec
descriptors, a complete format-19 payload verifier, and a deterministic
format-19 encoder with the original 29-level copy-match threshold table.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the current verifier milestone with:

```sh
build/replay-verify --codec 19 --verify-huffman
```

Verify a raw format-19 frame payload with no temporal dependencies:

```sh
build/replay-verify --codec 19 --payload frame.mb19 --size 320x256
```

Export decoded packed `Y,U,V` bytes and compare them with an Acorn reference:

```sh
build/replay-verify --codec 19 --payload frame.mb19 --size 320x256 \
    --previous-6y5uv previous.6y5uv --output-6y5uv decoded.6y5uv \
    --expect-6y5uv acorn-decoded.6y5uv
```

The payload verifier currently accepts all format-19 block modes. A payload
containing temporal references requires the library API so the caller can
supply the previous reconstructed frame, or the CLI's `--previous-6y5uv`
option. The CLI rejects temporal payloads when no reference is supplied rather
than inventing pixels. The original-codec corpus contract is documented in
`corpus/format19/README.md`.

Encode exactly one packed RGB24 frame as a raw format-19 payload:

```sh
ffmpeg -i input.mp4 -vf scale=320:256 -frames:v 1 \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size 320x256 \
    --payload frame.mb19 --trace frame.trace --recon-ppm frame.ppm
```

The encoder converts RGB with CompLib's non-dithered fixed-point path, decodes
every generated payload, and compares the result with its reconstructed frame
before writing the payload. Single-frame mode emits only 4x4 data blocks and
requires EOF after that frame.

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

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names:
  `codec_movingblocks`, `codec_movingblockshq`,
  `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.
