# ARMovie RO3.71 vs RO2003 High-Level Comparison

This note compares the two extracted ARMovie trees currently present in the
workspace:

- `ARMovie_RO371`: the RISC OS 3.71 ARMovie tree.
- `ARMovie_2003`: the 2003 snapshot ARMovie tree.

The goal is to identify where compression-relevant code is likely to live
before documenting individual codecs.

## Summary

`ARMovie_RO371` is much closer to a packaged application/resource tree. It has
261 files, mostly built RISC OS binaries and a small number of detokenised BBC
BASIC source files.

`ARMovie_2003` is a broader source tree. It has 1452 files and separates ARMovie
components into source-oriented areas such as `Video`, `Sound`, `Fetchers`,
`Painters`, `Colour`, `Filters`, and `Tools`. It contains many more codecs and
substantial C/assembler source for later video formats.

For compressor work, the 2003 tree is the better starting point. In particular,
`ARMovie_2003/Video/Decomp18` contains the H.263 compressor/decompressor sources
and a full `BatchComp` encoder tree.

## File-Type Mix

The files use RISC OS NFS naming conventions: `,xxx` suffixes represent file
types, and detokenised BBC BASIC appears as `,ffb.txt`.

Observed type counts:

| Tree | Total files | `ffd` | `ffb.txt` | `fd7` | `feb` | `ffa` | Other notable |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `ARMovie_RO371` | 261 | 163 | 28 | 3 | 2 | 0 | `ff9`, `ffe`, `ff8` |
| `ARMovie_2003` | 1452 | 386 | 234 | 100 | 13 | 9 | `ff5`, `ff9`, `ffe` |

At this level, the 2003 snapshot clearly contains more source/build material:
many more BASIC sources and many more `fd7` obey/build files.

## Layout Differences

### RO3.71

`ARMovie_RO371` is shallow:

- `Resources/Decomp2` through `Resources/Decomp9`, plus `Resources/Decomp17`
- `Resources/MovingLine`
- `Resources/Sound16`
- `Resources/Documents`
- `Resources/Shapes`
- `Resources/Tools`
- `bas`, `crunch`, `crunched`

The video codec directories under `Resources` usually contain built runtime
files such as `Decompress,ffd`, `DecompresH,ffd`, `Dec16,ffd`, etc. Some also
include detokenised BASIC generators or compressor front ends.

### RO2003

`ARMovie_2003` is source-organized:

- `Video`: many `DecompN` codec directories and `MovingLine`
- `Sound` and `Sound16`: many more audio codecs
- `Fetchers`: AVI, QuickTime/WSS, MovieFS stream, PNA, VPhone
- `Painters`: 1/2/4/8/16/32 bpp paint routines
- `Colour`, `Filters`, `Tools`, `Options`, `Docs`, `Resources`

Most video directories have a `Resources` subdirectory plus `bas` source, and
some add `crunch`, `Docs`, `c`, `h`, `hdr`, `s`, or full Makefiles.

## Overlapping Video Codecs

These codec/resource directories exist in both trees:

- `Decomp2`
- `Decomp3`
- `Decomp4`
- `Decomp5`
- `Decomp6`
- `Decomp7`
- `Decomp8`
- `Decomp9`
- `Decomp17`
- `MovingLine`

The overlap is not laid out identically. For example:

- `ARMovie_RO371/Resources/Decomp2` contains built files such as
  `Decompress,ffd`, `DecompresH,ffd`, `Info`, and `MakeDecomp,ffb.txt`.
- `ARMovie_2003/Video/Decomp2` contains `Resources/Info`,
  `bas/MakeDecomp,ffb.txt`, and `bas/BatchComp,ffb.txt`.

This pattern repeats for many older codecs: RO3.71 has packaged runtime
decompressors, while RO2003 has source/generator layout and not necessarily the
same built output files in the same place.

`Decomp7` and `Decomp17` are especially useful overlaps because the 2003 tree
adds stream documentation:

- `ARMovie_2003/Video/Decomp7/Docs/Stream`
- `ARMovie_2003/Video/Decomp17/Docs/Stream`

Those documents describe Moving Blocks bitstreams and should be used when
documenting those formats.

## Video Codecs Present Only in RO2003

The 2003 tree adds many video directories that are absent from the RO3.71
ARMovie resources:

- `Decomp10`, `Decomp11`, `Decomp12`
- `Decomp15`, `Decomp16`
- `Decomp18` through `Decomp27`
- `Decomp100`, `Decomp500`
- `Decomp600` through `Decomp610`
- `Decomp613` through `Decomp615`
- `Decomp622` through `Decomp630`
- `Decomp699`
- `Decomp800`, `Decomp802`
- `Decomp900`, `Decomp901`, `Decomp902`

The 2003 `Docs/Status` file gives useful names for many of these. Relevant
entries include:

- `Decomp7`: Moving Blocks, compressor and decompressor, BASIC/assembler.
- `Decomp17`: Moving Blocks HQ, compressor and decompressor, BASIC/assembler.
- `Decomp18`: H.263, compressor and decompressor, C/assembler.
- `Decomp19`: Super Moving Blocks, compressor and decompressor,
  BASIC/assembler.
- `Decomp20`: Moving Blocks Beta, compressor and decompressor, BASIC/assembler.
- `Decomp21` to `Decomp27`: additional YUV/delta-family Replay formats.
- `Decomp800` and `Decomp802`: later third-party/Pederson formats with some
  compressor source.

## Compression-Relevant Paths

Initial paths to inspect for compressor implementation:

- `ARMovie_2003/Video/Decomp18/BatchComp`
  - C batch compressor integration for H.263.
  - Contains `c`, `h`, `TMN`, and `RTcomp` subtrees.
- `ARMovie_2003/Video/Decomp18/BatchComp/TMN`
  - Telenor/TMN H.263 encoder code adapted for Replay.
- `ARMovie_2003/Video/Decomp18/Docs/bchcompdoc`
  - Documents the H.263 batch compressor and its changes from the Telenor code.
- `ARMovie_2003/Video/Decomp18/Docs/decompdoc`
  - Documents the H.263 decompressor adaptation.
- `ARMovie_2003/Video/Decomp18/Docs/h263`
  - H.263 reference/specification notes.
- `ARMovie_2003/Video/Decomp18/Docs/VidCmpTech,ff5`
  - Large video compression technical document.
- `ARMovie_2003/Video/Decomp7/Docs/Stream`
  - Moving Blocks stream format.
- `ARMovie_2003/Video/Decomp17/Docs/Stream`
  - Moving Blocks HQ stream format.
- `ARMovie_2003/Video/*/bas/BatchComp,ffb.txt`
  - BASIC compressor front ends/generators for non-H.263 formats.
- `ARMovie_RO371/Resources/*/BatchComp,ffb.txt`
  - RO3.71 packaged compressor front ends where present.

## Documentation Differences

Some documentation files exist in both trees and are identical:

- `DecompIf`
- `Extract`
- `Join`
- `PrefBig`

Some common documentation names differ between the trees:

- `AE7doc`
- `ProgIf`
- `ToCapture`
- `ToUseSound`

RO2003 also adds documentation not present in the RO3.71 resource documents,
including:

- `CodecIf`
- `ColourIf`
- `CompSound`
- `Fetchers`
- `Filters`
- `NewFormat2`
- `PaintRouts`
- `Docs/History`
- `Docs/Status`

`Docs/History` is useful for chronology. It records, among other things, the
introduction of `Decomp21` through `Decomp27`, changes to Moving Blocks
compressors, and a note that `Decomp18.BatchComp` was altered to handle the
`6Y5UV` colour space.

## Initial Interpretation

The two trees are not simple before/after versions of the same directory
structure. RO3.71 is mainly a compact application/resource snapshot with built
decompressors. RO2003 is a source archive containing the older Replay codecs,
many later codecs, codec documentation, source generators, tools, fetchers, and
build scripts.

For creating new C tooling to compress into Acorn Replay format, the likely
path is:

1. Use `ARMovie_2003/Docs/Status` to identify codec numbers and names.
2. Start with `Decomp18/BatchComp` for the most complete C encoder example.
3. Use `Decomp7` and `Decomp17` stream documents to understand the older Moving
   Blocks formats.
4. Cross-check older RO3.71 `BatchComp,ffb.txt` files where the 2003 tree has
   source-only packaging differences.
