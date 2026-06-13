# Type 20 (Moving Blocks Beta): two shipped revisions (old/new)

There are **two released revisions of the type 20 decompressor, with different
chroma formats.** Both are real -- the choice is which Decomp20 module the
target RISC OS system has installed:

| Variant | Module | Version | Chroma |
|---|---|---|---|
| **type 20 "old"** | `Decompress,ffd` **12456 bytes, 20 Sep 1996** (md5 `00d48529`); in `!Boot/Resources/!ARMovie` and the `!ARMovie_compiled` snapshot | v0.04, 6 Sep 1996 | **direct 6-bit** |
| **type 20 "new"** | `Decompress,ffd` **12528 bytes, 19 Nov 1996** (md5 `ccd887b4`); in `Replay/Replay3` and `Replay/OldARM` | v0.05, 19 Nov 1996 | **delta-coded** |

A third file -- `12532 bytes, 2026` (md5 `fb761251`, in `Public/ReplayComp` and
`RiscOs_2003/.../SystemRes/.../Decomp20`) -- is a *fresh build of the v0.05
source*. It is the same delta format as the 19 Nov shipped module; it is exactly
**4 bytes** larger only because the build appends 4 `0xff` tail-padding bytes
(the rest of the bytes differ because it is a separately assembled build, but
the on-wire format is identical). The disassembly confirms both new modules have
the `deltaexpand` table (at 0xd90) and the 10-bit delta data header.

The codec implements **both**: "old" (direct) is the default and matches the
20 Sep module that ships in `!Boot/Resources`; "new" (delta) matches the 19 Nov
module. A given `.ae7` movie just declares compression type 20, so its bytes
must match whichever Decomp20 the player has -- pick the variant accordingly.

## How the two differ on the wire

| | type 20 old (v0.04) | type 20 new (v0.05) |
|---|---|---|
| Decoder | `Decompress,ffd` 12456 bytes, 20 Sep 1996 | `Decompress,ffd` 12528 bytes, 19 Nov 1996 |
| Encoder | `BatchComp,ffb` 47469 bytes | `bas/BatchComp` + `GenD4Table` |
| Version (encoder + decoder) | **0.04, 6 Sep 1996** | **0.05, 19 Nov 1996** |
| Chroma | **direct 6-bit** | **delta-coded** |

The version strings are decisive: the 0.04 `BatchComp` declares
`"Version 0.04 6th September 1996"` and packs chroma directly (no `unpackuv`/
`deltatable`/`D4tab`/`prevu`, just `AND #63`); the 0.05 `BatchComp`/`MakeDecomp`
declare `"Version 0.05 19th November 1996"` and are delta-coded. The `!ARMovie_
compiled` snapshot happened to contain only the 0.04 decoder, which led to an
early (wrong) conclusion that the 0.05 source was never shipped -- in fact the
19 Nov 0.05 decoder ships in `Replay/Replay3` and `Replay/OldARM`.

Each version is internally consistent (no encoder/decoder mismatch): the 0.04
encoder and decoder are both direct; the 0.05 encoder and decoder are both
delta. The format is not selected by an `#ifdef` -- each `MakeDecomp` revision is
unconditionally one or the other (the 0.05 `MakeDecomp` is `ADD r0,#8+2 : BL
unpackuv` with no alternative path).

## The difference: chroma coding

Everything else is identical between the two — the block grammar (same as type
19), the format-19 motion tables, the 64-symbol luma Huffman table and 2-D luma
predictor, and the 6Y6UV output word (`Y | U<<6 | V<<12`, six bits each). Only
the **data-block chroma** differs:

**type 20 "old" (v0.04) — direct 6-bit chroma.**
Data header is `opcode(2) + U(6) + V(6) = 14 bits`. From the 12456-byte module:

```
0x08cc: add  r0, r0, #0xe        ; advance 14 bits (opcode + U + V)
0x08d0: lsl  r4, r5, #0x14       ; r4 = (low 12 bits of the post-opcode window)
0x08d4: lsr  r4, r4, #0xe        ;      << 6  ==  U<<6 | V<<12
```

So `U` is the 6-bit field at stream bits 2..7 and `V` at bits 8..13, stored
directly (signed mod 64). No predictor, no delta table.

**type 20 "new" (v0.05) — delta-coded chroma.**
Data header is `opcode(2) + uv-byte(8) = 10 bits`. From the 12528-byte module:

```
0x08d0: add  r0, r0, #0xa        ; advance 10 bits (opcode + uv byte)
0x08d4: bl   unpackuv
```

The uv byte holds a 4-bit delta code per component (u low nibble, v high nibble)
indexing

```
deltaexpand = { -32,-26,-20,-14,-8,-4,-2,-1, 0,1,2,4,8,14,20,26 }
```

added to a chroma predictor carried across data blocks (like the luma predictor)
and reset to zero each frame: `u = (prevu + deltaexpand[u_code])&63`. The
matching v0.05 `BatchComp` builds a `D4tab` (via `GenD4Table`) to pick the
closest delta code.

## Consequence

`codec_movingblocksbeta` implements **both** variants, selected by
`CodecMovingBlocksBetaVariant` (`...EncodeOptions.variant`,
`codec_movingblocksbeta_verify_frame_variant`). Each is cross-checked
byte-for-byte against its real module: "old" vs the 20 Sep 1996 12456-byte
`Decompress,ffd` (`test_decomp20_compiled`), "new" vs the 19 Nov 1996 12528-byte
module (`test_decomp20new_compiled`). The tooling exposes both:
`replay-encode --codec 20 --variant old|new`, and `replay-make --codec 20
[--variant new] [--type-alias N]` -- the alias writes compression type N in the
container so the "new" Decomp module can be installed at `Decomp<N>` (free
numbers 13, 14, 28, 29, 30) beside the "old" one without a directory clash.

A stream encoded for one variant will NOT decode on the other (the 14-bit vs
10-bit header alone desynchronises the luma), which is why the
cross-check infrastructure flagged the delta-coded first attempt: it was
self-consistent but produced streams the real (shipped) Decomp20 could not
decode. The mismatch was found by tracing the bitstream pointer under the
Unicorn harness (`tools/decomp19_unicorn.py --trace-bits`).
