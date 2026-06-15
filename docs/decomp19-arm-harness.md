# Decomp19 ARM Harness

> **Native harness:** `tools/replay_armsim.c` (the `replay-armsim` binary) is a
> dependency-free C replacement for both Python harnesses below. It runs the
> compiled module under the vendored ARMulator (`vendor/armulator`), which
> models the classic Acorn ARM memory semantics directly — an unaligned `LDR`
> rotates, and an unaligned `LDM`/word load ignores the low address bits — so
> **none of the per-codec instruction-signature alignment shims described below
> are needed**. It accepts the same options as `decomp19_unicorn.py` (plus
> `--codec 1` for Moving Lines, replacing `movinglines_unicorn.py`) and produces
> byte-identical output; the equivalence is covered by `test_armsim_corpus` and
> verified against the Unicorn harness across types 7/17/19/20/23 and Moving
> Lines. The reusable decode loop and pixel conversions live in
> `src/replay_codecif.c` (`replay/codecif.h`) so a future Replay→raw transcoder
> can share them. The decoders are 26-bit RISC OS modules and are run in 26-bit
> mode (they return with `MOVS pc, lr`); see `vendor/armulator/README.md`.

`tools/decomp19_unicorn.py` executes the original generated Moving Blocks
decompressors without RISC OS. Its historical name remains because type 19,
Super Moving Blocks was the first supported target. A compiled copy is present at
`!ARMovie_compiled/Decomp19/Decompress,ffd`; its SHA-256 digest is
`3d302da5f71efbc43a2cb677db75c4e46c791ec2d65bd272617e01e0656d8678`.

## Documented Interface

The authoritative general interface is `Resources/Documents/CodecIf`. The
type 19, Super Moving Blocks generator,
`Video/Decomp19/bas/MakeDecomp,ffb.txt`, confirms how that interface is used by
this particular codec.

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

`--codec 7` also supports the compiled type 7 (Moving Blocks) decoder. Its five
classic unaligned header loads use the same compatibility hook. The harness can
preserve raw ARM words with `--output-words-prefix`, initialize from a native
16-bit Replay key image with `--previous-words16`, or convert unpatched YUV555
words to 6Y5UV components with `--output-layout yuv555-to-6y5uv`.

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

To split a complete Replay chunk into exact type 19, Super Moving Blocks frame
payloads, ask the decoder to process the chunk sequentially:

```sh
tools/decomp19_unicorn.py \
    --decompressor Decompress,ffd \
    --payload chunk-000.video --size 160x128 --frames 25 \
    --output-prefix decoded/frame- --payload-prefix payload/frame-
```

The harness initializes the codec once, alternates two reconstruction buffers,
and passes each decoded frame back as the next frame's temporal reference. The
source pointer returned by one `CodecIf` call is the next frame's start, so the
saved `.mb19` files are decoder-established boundaries rather than inferred
sizes. The final summary reports any catalogue video bytes left after the
requested frame count.

## Remaining Validation

The general register contract comes from `CodecIf`; pixel packing and the
Decomp19 `r14` behavior are directly visible in `MakeDecomp,ffb.txt`.
`test_decomp19_compiled` cross-checks stationary, temporal, spatial, split, and
lossy payloads byte-for-byte against the portable verifier. It covers 4x4
temporal and spatial blocks plus temporal and spatial 2x2 blocks inside split
blocks. Original-compressor payload fixtures remain to be added.
