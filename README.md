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

## Naming

- Shared Moving Blocks files and public symbols use the `mb_` prefix.
- Codec-specific files use descriptive `codec_` names:
  `codec_movingblocks`, `codec_movingblockshq`,
  `codec_supermovingblocks`, and `codec_movingblocksbeta`.
- General Replay infrastructure uses the `replay_` prefix.

