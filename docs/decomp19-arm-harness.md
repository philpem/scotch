# Decomp19 ARM Harness

`tools/decomp19_unicorn.py` executes the original generated format-19
decompressor without RISC OS. A compiled copy is present at
`!ARMovie_compiled/Decomp19/Decompress,ffd`; its SHA-256 digest is
`3d302da5f71efbc43a2cb677db75c4e46c791ec2d65bd272617e01e0656d8678`.

## Documented Interface

The authoritative general interface is `Resources/Documents/CodecIf`. The
format-19 generator, `Video/Decomp19/bas/MakeDecomp,ffb.txt`, confirms how that
interface is used by this particular codec.

The generated file starts with three words, as specified by `CodecIf`:

- offset `0`: patch-table offset;
- offset `4`: branch to the init entry;
- offset `8`: decompress-one-frame entry.

Init receives width in `r0` and height in `r1`. `CodecIf` defines `r2="PARM"`
plus an `r3` parameter-list pointer when parameters are present. This codec has
none, so the harness explicitly sets both registers to zero. It does not issue
the optional later `r2="SHUT"` finalisation call because Decomp19 allocates no
external state.

Decode receives the source byte pointer in `r0`, word-aligned output pointer in
`r1`, previous-frame pointer in `r2`, colour lookup pointer in `r3`, and return
address in `r4` for the default call sequence. `CodecIf` reserves `r14` as the
return address for the C calling sequence. The compiled Decomp19 `Info` file's
bit-depth/calling line is `,C`, so `r14` is authoritative for this codec and the
generated entry saves it internally. The older interface comment retained in
`MakeDecomp` still names `r4`; the harness supplies the same sentinel there as
a harmless compatibility measure.

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

The decoder uses unaligned `LDMIA` lookahead loads for four block/sub-block
headers and two Huffman paths. Classic Acorn ARM cores ignore the low two
address bits for these multiple loads; Unicorn uses the literal unaligned
address even when configured as its oldest available ARM models. The harness
therefore intercepts those source-derived instruction signatures and performs
aligned loads before allowing execution to continue. Without this compatibility
shim, a variable-length block can shift the following opcode or Huffman value.

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

The general register contract comes from `CodecIf`; pixel packing and the
Decomp19 `r14` behavior are directly visible in `MakeDecomp,ffb.txt`.
`test_decomp19_compiled` cross-checks stationary, temporal, spatial, split, and
lossy payloads byte-for-byte against the portable verifier. It covers 4x4
temporal and spatial blocks plus temporal and spatial 2x2 blocks inside split
blocks. Original-compressor payload fixtures remain to be added.
