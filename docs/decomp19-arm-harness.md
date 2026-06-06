# Decomp19 ARM Harness

`tools/decomp19_unicorn.py` executes the original generated format-19
decompressor without RISC OS. The 2003 snapshot contains its BBC BASIC source
generator but does not contain the generated `Video.Decomp19.Decompress` file,
so that binary must first be produced under RISC OS.

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
return address for the C calling sequence. Decomp19's generated entry saves
`r14` internally even though its source comments describe the default `r4`
interface, so the harness supplies the same sentinel return address in both
registers. This is a codec-specific accommodation, not a redefinition of the
documented interface.

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

The general register contract comes from `CodecIf`; pixel packing and the
Decomp19 `r14` behavior are directly visible in `MakeDecomp,ffb.txt`. The
harness cannot be run against the real codec until the missing generated binary
is supplied. The first execution should use a one-block data payload, then
stationary, temporal, spatial, and split fixtures. Preserve the generated
decompressor's digest and RISC OS source snapshot in corpus provenance.
