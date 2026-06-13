# Super Moving Blocks Compressor Display

This note documents the display produced by the Acorn type 19, Super Moving
Blocks `BatchComp` compressor.

## Main Images

The matching loop describes its three frame buffers as:

```text
b1  current/previous reconstructed frame
b2  new source frame being compressed
b3  reconstructed output frame being built
```

After each compression attempt, the display code calls:

```text
PROCPaintRectangleR(b2%)
PROCPaintRectangleL(b3%)
```

Therefore:

- **left:** `b3`, the reconstructed type 19, Super Moving Blocks output;
- **right:** `b2`, the new source frame against which matches are evaluated.

The left image is the important encoder feedback image. It contains exactly
the quantised/copied pixels retained for future temporal references, rather
than an uncompressed preview of the source.

## Coloured Block Display

The source also contains a debug block map, conditional on `debug%`. The
checked-in 2003 detokenised source sets `debug%=FALSE`, so a runnable compressor
which shows the map was built or run from a debug-enabled variant. Depending on
the build and screen layout, it may look like coloured squares over or adjacent
to the left image. The writes use the 15-bit RGB component fields directly:

| Colour | Source action | Meaning |
|---|---|---|
| Green | `MOV r5,#31<<5` after `findblk16` | An accepted 4x4 copy match. The colour does not distinguish stationary, temporal, and spatial copies. |
| Blue | `MOV r5,#31<<10` in `sample16` | A literal/data-coded 4x4 block was reconstructed. |
| Black | Clear at entry to `find4` | The 4x4 block is being represented as four 2x2 sub-blocks; black is also left for a literal/data-coded 2x2 quadrant. |
| Red | `MOVNE r5,#31` for a `findblk4` result | That 2x2 quadrant used an accepted copy match. As with green, it does not distinguish stationary, temporal, and spatial. |
| White/black | Separate `zm%`/screen marker | Records whether a whole-block 4x4 match was unavailable/available while the compressor considered split versus literal coding. It is diagnostic search state, not another output mode. |

Consequently, a red-and-black 4x4 square is a split block whose quadrants mix
copy and literal 2x2 decisions. Green and blue describe whole 4x4 choices.

The inspected source does **not** explicitly write a magenta classification.
Observed magenta is most likely a palette/display-mode effect, stale or
overlapping diagnostic pixels during retries, or visual mixing at the display
scale. It should not be interpreted as a distinct type 19, Super Moving Blocks
opcode without a captured screen and build-specific evidence.

## Retry Behaviour

`PROCmatch` can redraw the display several times for one source frame while it
changes the quality level to meet its byte window. The displayed block map may
therefore show an intermediate attempt briefly. The final log entry and final
`b3` reconstruction correspond to the accepted attempt.

## Source References

- `Video/Decomp19/bas/BatchComp,ffb.txt`, `PROCmatch`: buffer roles and left/
  right paint calls.
- `sample16`: blue 4x4 literal marker.
- `findblk16`: green accepted 4x4 match marker.
- `find4`: black split background and red accepted 2x2 match quadrants.
- `bas/CompLib,ffb.txt`: `PROCPaintRectangleL`, `PROCPaintRectangleR`, and
  screen placement.
