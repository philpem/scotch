# Type 20 (Moving Blocks Beta): shipped module vs ARMovie_2003 source

The released Decomp20 decompressor and the Decomp20 source in `ARMovie_2003`
are **two different revisions of a beta codec, and they use different chroma
formats.** This codec is implemented to match the *shipped* module, because
that is what RISC OS Replay players (and emulators) actually load.

## Which is which

| | Shipped module | ARMovie_2003 source |
|---|---|---|
| Decoder | `Decompress,ffd` 12456 bytes, 20 Sep 1996 | (not compiled; built by `MakeDecomp`) |
| Encoder | `BatchComp,ffb` 47469 bytes | `bas/BatchComp` + `GenD4Table` |
| Version (both encoder + decoder) | **0.04, 6 Sep 1996** | **0.05, 19 Nov 1996** |
| Chroma | **direct 6-bit** | **delta-coded** |
| Status | The released module on player/emulator installs | A *later*, unreleased revision |

The version strings are decisive: the shipped `BatchComp` declares
`"Version 0.04 6th September 1996"` and the ARMovie_2003 `BatchComp` declares
`"Version 0.05 19th November 1996"`. So the released binary was built from an
**earlier (0.04) revision** than the source tree carries (0.05); between the two
the author switched chroma from direct to delta-coded, and that 0.05 revision was
never compiled into a shipped module (there is no compiled `Decompress` in the
source tree, only the compressor in `crunch`).

This rules out two tempting explanations:

* **Not an `#ifdef`/conditional.** The 0.05 `MakeDecomp` is *unconditionally*
  delta-coded -- `ADD r0,#8+2 : BL unpackuv` with no `IF` around it and no
  alternative direct path -- so that source cannot generate the direct-chroma
  `Decompress`.
* **Not an encoder/decoder mismatch.** Each version is internally consistent:
  the shipped **0.04 encoder** (`BatchComp`) has no `unpackuv`/`deltatable`/
  `D4tab`/`prevu` and packs chroma directly (`AND #63`), matching the shipped
  0.04 decoder; the 0.05 source encoder and decoder are *both* delta-coded.

## The difference: chroma coding

Everything else is identical between the two — the block grammar (same as type
19), the format-19 motion tables, the 64-symbol luma Huffman table and 2-D luma
predictor, and the 6Y6UV output word (`Y | U<<6 | V<<12`, six bits each). Only
the **data-block chroma** differs:

**Shipped module — direct 6-bit chroma (implemented here).**
Data header is `opcode(2) + U(6) + V(6) = 14 bits`. From the disassembly of the
data-block path:

```
0x08cc: add  r0, r0, #0xe        ; advance 14 bits (opcode + U + V)
0x08d0: lsl  r4, r5, #0x14       ; r4 = (low 12 bits of the post-opcode window)
0x08d4: lsr  r4, r4, #0xe        ;      << 6  ==  U<<6 | V<<12
```

So `U` is the 6-bit field at stream bits 2..7 and `V` at bits 8..13, stored
directly (signed mod 64). No predictor, no delta table.

**ARMovie_2003 source — delta-coded chroma (NOT shipped).**
Data header is `opcode(2) + uv-byte(8) = 10 bits` (`ADD r0,#8+2; BL unpackuv`).
The uv byte holds a 4-bit delta code per component (u low nibble, v high
nibble) indexing

```
deltaexpand = { -32,-26,-20,-14,-8,-4,-2,-1, 0,1,2,4,8,14,20,26 }
```

which is added to a chroma predictor carried across data blocks (like the luma
predictor) and reset to zero each frame: `u = (prevu + deltaexpand[u_code])&63`.
The matching `BatchComp` builds a `D4tab` (via `GenD4Table`) to pick the closest
delta code, and adds a `hiq%` high-quality cost mode. None of this is in the
released decompressor.

## Consequence

`codec_movingblocksbeta` implements the **shipped** (direct 6-bit) format and is
cross-checked byte-for-byte against the 20 Sep 1996 `Decompress,ffd`
(`test_decomp20_compiled`, `test_fullmovie_decomp20`). A stream encoded for the
delta-coded source revision would NOT decode on the shipped module (the 14-bit
vs 10-bit header alone desynchronises the luma), which is exactly why the
cross-check infrastructure flagged the delta-coded first attempt: it was
self-consistent but produced streams the real (shipped) Decomp20 could not
decode. The mismatch was found by tracing the bitstream pointer under the
Unicorn harness (`tools/decomp19_unicorn.py --trace-bits`).
