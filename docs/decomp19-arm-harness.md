# Decomp19 ARM Harness

`tools/decomp19_unicorn.py` executes the original generated format-19
decompressor without RISC OS. The 2003 snapshot contains its BBC BASIC source
generator but does not contain the generated `Video.Decomp19.Decompress` file,
so that binary must first be produced under RISC OS.

## Source-Derived Interface

The generated file starts with three words:

- offset `0`: patch-table offset;
- offset `4`: branch to the init entry;
- offset `8`: decompress-one-frame entry.

Init receives width in `r0` and height in `r1`. Decode receives the source byte
pointer in `r0`, output pointer in `r1`, previous-frame pointer in `r2`, colour
lookup pointer in `r3`, and return address in `r4`. The harness also supplies
the return address in `r14` because the generated routine saves it on entry.

The patch table is intentionally left untouched. Its placeholder instructions
are register-to-same-register moves, so the 32-bit output remains:

```text
bits  0..5   Y
bits  6..10  U, modulo five-bit representation
bits 11..15  V, modulo five-bit representation
bits 16..31  zero
```

The harness converts those words to the corpus's packed byte triplets. Previous
frames are converted in the opposite direction before execution.

## Use

```sh
tools/decomp19_unicorn.py \
    --decompressor Decompress,ffd \
    --payload frame.mb19 --size 320x256 \
    --previous previous.6y5uv \
    --output acorn-decoded.6y5uv

build/replay-verify --codec 19 --payload frame.mb19 --size 320x256 \
    --previous-6y5uv previous.6y5uv \
    --expect-6y5uv acorn-decoded.6y5uv
```

Omit `--previous` for a key frame. The harness allocates a zero previous frame
in that case; a valid key-frame payload must not read it.

## Remaining Validation

The register and packing contract is directly visible in
`MakeDecomp,ffb.txt`, but the harness cannot be run until the missing generated
binary is supplied. The first execution should use a one-block data payload,
then stationary, temporal, spatial, and split fixtures. Preserve the generated
decompressor's digest and RISC OS source snapshot in corpus provenance.
