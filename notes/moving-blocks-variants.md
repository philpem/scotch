# Moving Blocks Variants

This note maps the Moving Blocks family before drilling into each later
bitstream in full detail.

Sources read so far:

- `ARMovie_2003/Resources/Documents/AE7doc`
- `ARMovie_2003/Video/Decomp7/Docs/Stream`
- `ARMovie_2003/Video/Decomp17/Docs/Stream`
- `ARMovie_2003/Video/Decomp*/Resources/Info`
- `ARMovie_2003/Video/Decomp*/bas/BatchComp,ffb.txt` for `Decomp7`,
  `Decomp17`, `Decomp19`, and `Decomp20`
- `ARMovie_2003/Video/Decomp*/bas/MakeDecomp,ffb.txt` for `Decomp17`,
  `Decomp19`, and `Decomp20`

`Decomp19` and `Decomp20` do not appear to have separate stream-format
documents in this tree, so their raw-data coding still needs source-level
documentation from the compressor and decompressor.

## Format Inventory

| Type | Directory | Name | Source version | Colour space | Notes |
| ---: | --- | --- | --- | --- | --- |
| 7 | `Decomp7` | Moving Blocks | 1993 format, compressor source in 2003 tree | `YUV 5,5,5` | Original documented Moving Blocks format. |
| 17 | `Decomp17` | Moving Blocks HQ | `Version 0.03 27th August 1996` | `YUV 5,5,5` | Newer Moving Blocks coding structure, documented by `Docs/Stream`. |
| 19 | `Decomp19` | Super Moving Blocks | `Version 0.04 6th September 1996` | `6Y5UV 6,5,5` | HQ-style compressor with 6-bit luma and 5-bit chroma. |
| 20 | `Decomp20` | Moving Blocks Beta | `Version 0.05 19th November 1996` | `6Y6UV 6,6,6` | HQ-style compressor with 6-bit luma/chroma and extra delta/chroma machinery. |

The numbering is not chronological by the source version strings: `Decomp19`
is dated September 1996 and `Decomp20` is dated November 1996, despite
`Decomp20` being named "Beta".

## Shared Model

All four variants are still recognisably the same codec family:

- frames are scanned as 4x4 blocks from top left to bottom right;
- the compressor builds a reconstructed output frame while encoding;
- future temporal matches use that reconstructed frame, not the original source;
- a block can be coded as raw data, a temporal copy, a spatial copy, or four
  2x2 sub-block decisions;
- spatial copies are only from already-decoded pixels in the current frame;
- temporal copies are from the previous decoded frame;
- the compressor retries a whole frame at different `qual%` settings to hit a
  byte budget;
- the `QP%` table still drives match tolerance through `maxi`, `maxe`,
  `tot16`, and `tot4`.

The important conceptual distinction is that the codec is not just finding
similar source pixels. It is always matching against values that the player can
reconstruct from the bitstream. That is what keeps subsequent temporal frames
in sync with Replay's decoder.

## Format 7: Moving Blocks

`Decomp7` is the simplest Moving Blocks variant and has its own exact note in
`notes/moving-blocks-format7-bitstream.md`.

High-level properties:

- compression type `7`;
- `YUV 5,5,5`;
- fixed 90-bit raw 4x4 blocks: 16 Y values plus one U and one V;
- fixed 30-bit raw 2x2 blocks: 4 Y values plus one U and one V;
- temporal search radius is 4 pixels (`move%=4`);
- no explicit top-level stationary block code;
- a stationary block is encoded as a move/copy with temporal offset `(0,0)`;
- the stream document's top-level code is `1` raw 4x4, `00` move, `01` split.

Its default bitrate window is a fixed byte range around the current target:

- `framesize +/- 500` below 25 fps;
- `framesize +/- 250` at 25 fps and above.

In `-quality` mode its starting frame-size estimate is:

```basic
framesize = sz% / 16 * 6
```

## Format 17: Moving Blocks HQ

`Decomp17` remains `YUV 5,5,5`, but it is not the same bitstream as format 7.
Its stream document changes the top-level and 2x2 case codes.

Top-level 4x4 cases in `Decomp17/Docs/Stream` are:

| Code | Meaning |
| --- | --- |
| `00` | 4x4 stationary block |
| `01` | 4x4 move case |
| `10` | 4x4 data |
| `11` | split into four 2x2 cases |

The four 2x2 cases use:

| Code | Meaning |
| --- | --- |
| `00` | 2x2 stationary block |
| `01` | 2x2 data |
| `1` | 2x2 move case |

Compared with format 7, HQ adds explicit stationary cases. This should make
unchanged or nearly unchanged video cheaper because the common no-motion case no
longer needs to be expressed through the general motion-code table.

The source also shows a broader motion-search setup:

```basic
xmove%=8:ymove%=8:typea%=FALSE
```

So the compressor can search up to an 8-pixel Chebyshev radius. The stream
document describes near motion with fixed families for distances 1 and 2,
then a distance 3/4 or spatial family, then a "rest of codes" family.

`Decomp17` also introduces a Huffman table for raw/sample coding:

```basic
PROCReadHuffTable
DIM hufftable% 95*4
FNhuffcode(...)
FNhufflen
```

The decompressor confirms this is not just compressor bookkeeping:
`MakeDecomp,ffb.txt` sets `maxbits%=9`, builds a Huffman decode table, patches
the runtime pointer named `hufftable`, and has decode paths that load from that
table.

This means raw 4x4 and 2x2 data should not be treated as the simple fixed
90-bit and 30-bit coding used by format 7, even though the stream document
still describes 5-bit Y, U, and V component values. The exact raw/sample coding
needs a dedicated pass through `sample16%`, `sample4%`, `test16%`, the Huffman
table, and the decompressor.

HQ also changes target sizing. In `-quality` mode:

```basic
framesize = sz% / 16 * 6 * .75
```

In normal data-rate mode it uses a relative target window:

```basic
uplim%   = downlim% - framesize / 20
downlim% = downlim% + framesize / 20
```

That is approximately a `+/- 5%` window around the current adjusted target,
rather than format 7's fixed `+/- 250` or `+/- 500` bytes.

## Format 20: Moving Blocks Beta

`Decomp20` is named "Moving Blocks Beta" and advertises:

```text
6Y6UV 6,6,6
```

The compressor explicitly asks CompLib to convert source frames to that format:

```basic
convertto$="6Y6UV"
```

The broad structure is HQ-like:

- `xmove%=8:ymove%=8:typea%=FALSE`;
- the same `QP%` values as formats 7, 17, and 19;
- the same relative `framesize / 20` bitrate window as HQ;
- a 128-entry Huffman table;
- a decompressor Huffman table with `maxbits%=11`;
- `testcount%` is used when deciding whether four 2x2 decisions beat a raw
  4x4 block, rather than comparing against a fixed 90-bit raw cost.

The distinctive `Decomp20` features seen so far are:

- all Y, U, and V components are handled as 6-bit quantities;
- it loads a `D4tab` table:

```basic
OSCLI"Load <ARMovie$Dir>.Decomp20.D4tab "+STR$~deltatable
```

- it maintains extra previous-chroma state while emitting/testing raw data:

```basic
!prev=0:?prevu=0:?prevv=0
initprevuv%=!prevu
```

The source comments near `prevu` and `prevv` say that they need to be
saved/restored when trying sub-4x4s. That strongly suggests Beta's raw/chroma
coding is predictive or delta-coded in a way that makes the coding state part
of the 2x2-versus-4x4 decision. The exact interpretation of `D4tab`,
`prevu`, and `prevv` still needs to be derived from both compressor and
decompressor code.

## Format 19: Super Moving Blocks

`Decomp19` advertises:

```text
6Y5UV 6,5,5
```

and the compressor sets:

```basic
convertto$="6Y5UV"
```

Its structure is very close to `Decomp20`:

- `xmove%=8:ymove%=8:typea%=FALSE`;
- same `QP%` values;
- same relative `framesize / 20` bitrate window;
- same 128-entry Huffman table size;
- same decompressor Huffman table size as `Decomp20` (`maxbits%=11`);
- same `testcount%` mechanism for comparing 2x2 subdivision against raw 4x4
  coding.

The obvious difference from `Decomp20` is component precision:

- Y is 6-bit;
- U and V are 5-bit;
- there is no `D4tab` load in the compressor source;
- the frame retry loop resets only `!prev=0`, not `prevu`/`prevv`.

So `Decomp19` currently looks like the production "Super" version of the
HQ/Beta line for the `6Y5UV` colour pipeline: it keeps the larger search,
newer case coding, Huffman/raw-cost machinery, and 6-bit luma, but drops
Beta's 6-bit chroma and explicit `D4tab`/previous-chroma state.

## Quality And Bitrate Behaviour

The same `QP%` table appears in all four compressors. Larger `qual%` still
means looser matching:

- larger per-component tolerances;
- larger total allowed 4x4 and 2x2 errors;
- usually smaller output;
- usually lower visual fidelity.

The user-facing name `quality` is therefore easy to misread. Internally,
raising `qual%` is how the compressor degrades a frame to fit the target.

The later formats change the bitrate control in two important ways:

- `Decomp17`, `Decomp19`, and `Decomp20` estimate quality-mode frame size as
  75% of the format-7 estimate;
- their default target window is proportional to `framesize`, approximately
  `+/- 5%`, rather than a fixed byte margin.

The later formats also have better coding tools: explicit stationary cases,
larger motion search, and Huffman/raw-cost handling. Those changes explain why
the target-size estimate can be lower without necessarily implying worse
results for the same material.

## What Not To Assume Yet

Do not use the exact format-7 bitstream as a substitute for formats 17, 19, or
20.

Known differences:

- format 17 has different top-level and 2x2 decision codes;
- formats 17, 19, and 20 use Huffman-coded sample machinery in the compressor;
- format 19 changes the source representation to `6Y5UV`;
- format 20 changes the source representation to `6Y6UV` and adds `D4tab` plus
  previous U/V state.

Exact bitstream notes now exist for formats 17, 19, and 20:

- `notes/moving-blocks-format17-bitstream.md`
- `notes/moving-blocks-format19-bitstream.md`
- `notes/moving-blocks-format20-bitstream.md`

The current implementation recommendation is to target format 19 first; see
`notes/moving-blocks-next-implementation-target.md`.
