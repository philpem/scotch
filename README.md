# Replay Tooling

Portable C tooling for inspecting, verifying, and encoding Acorn Replay video
streams.

The first implemented target is compression type 19, Super Moving Blocks. The
current milestone provides byte/bitstream primitives, Moving Blocks codec
descriptors, a complete format-19 payload verifier, and a deterministic
data-only format-19 encoder.

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

The payload verifier currently accepts all format-19 block modes. A payload
containing temporal references requires the library API so the caller can
supply the previous reconstructed frame; the standalone CLI intentionally
rejects such a payload rather than inventing reference pixels.

Encode exactly one packed RGB24 frame as a raw format-19 payload:

```sh
ffmpeg -i input.mp4 -vf scale=320:256 -frames:v 1 \
    -pix_fmt rgb24 -f rawvideo - | \
    build/replay-encode --codec 19 --input - --size 320x256 \
    --payload frame.mb19 --trace frame.trace --recon-ppm frame.ppm
```

This first encoder emits only 4x4 data blocks. It converts RGB with CompLib's
non-dithered fixed-point path, decodes every generated payload, and compares
the result with its reconstructed frame before writing the payload. The CLI
requires EOF after one frame; sequence framing belongs in the later Replay
container layer.

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names:
  `codec_movingblocks`, `codec_movingblockshq`,
  `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.
