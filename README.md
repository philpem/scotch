# Replay Tooling

Portable C tooling for inspecting, verifying, and eventually encoding Acorn
Replay video streams.

The first implemented target is compression type 19, Super Moving Blocks. The
current milestone provides byte/bitstream primitives, Moving Blocks codec
descriptors, the format-19 luma Huffman table, and verifier-focused tests.

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

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names:
  `codec_movingblocks`, `codec_movingblockshq`,
  `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.

