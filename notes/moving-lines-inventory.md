# Moving Lines Inventory

This is the starting inventory for understanding Acorn Replay compression type
1, `Moving Lines`.

## Identity

Both RO3.71 and the 2003 tree describe the codec as:

```text
Moving Lines
Acorn Computers 1993

1;1;1280
1;1;1024
Temporal,Spatial
YUV 5,5,5; RGB 5,5,5
```

The compression type is `1`. CompLib special-cases this type by using the
`MovingLine` directory rather than a `Decomp1` directory.

The batch compressor version string is the same in both trees:

```basic
MovingLines Batch Compressor v 0.24 08th April 1994
```

## Main Source Paths

RO3.71:

- `ARMovie_RO371/Resources/MovingLine/Info`
- `ARMovie_RO371/Resources/MovingLine/BatchComp,ffb.txt`
- `ARMovie_RO371/MakeDecomp,ffb.txt`
- `ARMovie_RO371/Resources/MovingLine/Make4col11,ffb.txt`
- `ARMovie_RO371/Resources/MovingLine/Make8col11,ffb.txt`

2003:

- `ARMovie_2003/Video/MovingLine/Resources/Info`
- `ARMovie_2003/Video/MovingLine/bas/BatchComp,ffb.txt`
- `ARMovie_2003/Video/MovingLine/bas/MakeDecomp,ffb.txt`
- `ARMovie_2003/Video/MovingLine/crunch/BatchComp,ffe`

The 2003 tree does not appear to carry the generated runtime tables beside the
source in `Video/MovingLine`; the RO3.71 `Resources/MovingLine` directory does.

## Runtime Files Seen In RO3.71

RO3.71 includes these generated/runtime files under
`ARMovie_RO371/Resources/MovingLine`:

- `Decompress,ffd`
- `8rgb11,ffd`
- `8rgb22,ffd`
- `8yuv11,ffd`
- `8yuv22,ffd`
- `16yuv,ffd`
- `32yuv,ffd`
- `8DefCol,ffd`
- sound playback helpers and ADPCM helpers

The table names match the player and CompLib code paths that load different
display/decompression support depending on screen mode, colour model, and scale.

The 2003 `MakeDecomp` source saves several decompressor variants:

- `Decompress`
- `DecompresH`
- `DecompresB`

Those names mirror the later `DecompN` codec directories.

## Source Size

Line counts:

```text
  959 ARMovie_2003/Video/MovingLine/bas/BatchComp,ffb.txt
  942 ARMovie_2003/Video/MovingLine/bas/MakeDecomp,ffb.txt
  834 ARMovie_RO371/Resources/MovingLine/BatchComp,ffb.txt
  321 ARMovie_RO371/MakeDecomp,ffb.txt
```

The 2003 `BatchComp` appears to be the same v0.24 compressor with more spacing
and comments. It is the better first source for reading, while RO3.71 is useful
for runtime files and historical comparison.

## First Compressor Observations

The compressor is CompLib-based:

```basic
LIBRARY "<ARMovie$Dir>.Tools.CompLib"
PROCStartup(1)
```

It uses CompLib's normal source expansion and chunk-writing callbacks. The
frame loop is structurally similar to Moving Blocks:

```basic
PROCExpandNextSourceFrame(frame%)
PROCmatch
SWAP b1%,b3%
PROCCheckChunkFinished(b1%)
```

Important early variables:

- `b1%`: current/previous reconstructed output frame.
- `b3%`: next reconstructed output frame.
- `b4%`: temporary frame buffer.
- `move%=8`: temporal search radius setup.
- `smove%=9`: spatial movement/table setup.
- `numtables%=36`: number of quality/threshold tables.
- `tabs%`: generated threshold/table storage.
- `tar%`: per-quality target values.

The first frame disables temporal matching:

```basic
rle%=8:E%=both%:IFframe%=0 E%=spatial%:rle%=2
```

That matches the `Temporal,Spatial` model already documented for the player:
temporal references use the previous decoded frame, while spatial references
use already-available output in the current frame.

## Bitstream Clues

The decompressor comments identify a combined temporal/spatial offset table:

```basic
r5 - temporal table of line offsets (0-&11f) plus spatial offsets (&120-&1cb)
```

The compressor emits 16-bit words with helper procedures such as `PROCopw` and
`PROCopdw`. A dummy frame emits:

```basic
PROCr
PROCopw(1+(&1cc<<7)):REM complete frame
```

The debug/decode path in `PROCmatch` interprets codes roughly as:

- bit 0 set means a run/copy style code;
- code field `C% < &120` means temporal table copy;
- `&120 <= C% < &1cc` means spatial copy;
- `C% >= &1e0` enters extended/raw handling;
- `&1cc` is used as the complete-frame marker.

This needs a dedicated bitstream pass before being treated as exact.

The first-pass code-word families are now documented in
`notes/moving-lines-bitstream-first-pass.md`.

The first-pass compressor decision process is documented in
`notes/moving-lines-compressor-process.md`.

The decompressor dispatch, offset table, and output-size variants are
documented in `notes/moving-lines-decompressor.md`.

## Rate Control

Moving Lines uses the same broad CompLib retry idea as Moving Blocks:

- derive `framesize` from data rate, sound bytes, chunk timing, and frame count;
- calculate `uplim%` and `downlim%`;
- run `PROCmatch`;
- adjust `qual%` and retry until the frame fits or the escape conditions fire.

Its default target window differs from Moving Blocks:

- below 25 fps: `downlim +/- 300` bytes;
- 25 fps and above: `downlim +/- 150` bytes.

The initial default quality is source-dependent:

```basic
IF sourceisyuv% qual%=7 ELSE qual%=4
```

## Next Reading Order

1. Compare RO3.71 `BatchComp` against the 2003 source for semantic differences,
   ignoring formatting-only churn.
2. Compare generated decompressor code or runtime binaries where source return
   sequences differ.
