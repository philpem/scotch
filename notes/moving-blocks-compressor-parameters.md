# Moving Blocks Compressor Parameters

This note records how the original Moving Blocks compressors turn user and
CompLib settings into block decisions and frame sizes. Formats 7, 17, 19, and
20 share more parameter behaviour than their bitstreams suggest.

## Quality Is A Loss Level

The BASIC variable is `qual%`, but increasing it permits more error and usually
reduces output size. A portable implementation should call it `loss_level`
internally and may retain `--quality` only as a compatibility alias.

All four compressors use the same 29-entry table, levels `0..28`. Each entry
contains:

| Field | Meaning |
| --- | --- |
| `maxi` | ordinary per-component difference accepted by a match |
| `maxe` | exceptional/larger per-component difference limit |
| `tot16` | total error limit for a 4x4 candidate |
| `tot4` | total error limit for a 2x2 candidate |

Important points in the table are:

| Loss level | `maxi` | `maxe` | `tot16` | `tot4` |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 1 | 0 | 1 | 2 | 1 |
| 4 | 1 | 1 | 8 | 2 |
| 7 | 1 | 2 | 14 | 4 |
| 10 | 1 | 2 | 20 | 5 |
| 14 | 1 | 3 | 36 | 9 |
| 18 | 2 | 4 | 72 | 18 |
| 22 | 3 | 7 | 132 | 33 |
| 25 | 5 | 10 | 180 | 45 |
| 28 | 6 | 13 | 216 | 60 |

The table is monotonic in permissiveness but not smoothly scaled. The first C
implementation should copy it exactly and expose the four resolved thresholds
in trace output.

## Initial Level And Retry Direction

The original compressors begin at `qual%=7` unless surrounding options or a
previous frame influence the value.

They encode a whole frame, compare its byte count with an allowed window, and
retry:

- output too large: increment `qual%`, accepting more approximate matches;
- output too small: decrement `qual%`, requiring closer matches;
- output in the window: accept the frame;
- level at an endpoint: accept the best achievable result or use fallback
  handling from the existing retry loop.

Retries must restore output position, predictor state, and current reconstructed
frame. Format 20 must also reset `prevu` and `prevv`.

## Frame-Size Target

CompLib derives an average video budget from data rate, chunk duration, buffer
policy, and sound bytes. A manually supplied frame size overrides that result.

For ordinary frames, the initial acceptance window is approximately:

```text
lower size bound = 0.90 * target frame bytes
upper size bound = 1.025 * target frame bytes
```

The BASIC names are counterintuitive: `uplim%` is the smaller number and
`downlim%` is the larger number. Portable code should instead use
`target_min_bytes` and `target_max_bytes`.

Formats 17, 19, and 20 later expand the current upper allowance by roughly
`target/20` on each side. Format 7 has fixed 250/500-byte adjustments in some
paths. These policies belong in rate control, not the format descriptor.

The available space for later frames can include unused bytes from earlier
frames in the chunk. This lets the original compressor aim at a chunk budget
rather than forcing every frame to exactly the same size.

## Search Range

| Format | Temporal search | Spatial search |
| ---: | --- | --- |
| 7 | `move%=4`, a 9x9 neighbourhood | `smove%=9` in the original search code |
| 17 | `xmove%=8`, `ymove%=8` | legal spatial entries from the format tables |
| 19 | `xmove%=8`, `ymove%=8` | same later Moving Blocks table structure |
| 20 | `xmove%=8`, `ymove%=8` | same later Moving Blocks table structure |

Search range affects encoding time and the chance of finding a cheap copy. It
does not alter the decoder interface as long as the emitted motion code is
legal for the selected format.

The first C encoder should use each format's full legal table. Later speed
options may search a subset without changing the bitstream.

## Candidate Controls

Useful portable controls and their effects:

| Control | Effect |
| --- | --- |
| `loss_level` | selects the four match thresholds; larger means lossier and usually smaller |
| `target_bytes_per_frame` | drives whole-frame retries; does not directly allocate bits to blocks |
| `enable_stationary` | permits same-position previous-frame copies |
| `enable_temporal` | permits displaced previous-frame copies |
| `enable_spatial` | permits copies from reconstructed pixels earlier in the current frame |
| `enable_split_2x2` | permits four smaller decisions instead of one 4x4 decision |
| `search_radius_x/y` | limits candidates considered by the encoder, never beyond format legality |

Disabling a candidate class should still produce a valid stream; it only makes
the encoder less effective. `--data-only` disables all copy candidates and is
the principal bring-up and verification mode.

## Chroma And Luma Parameters

The bitstream fixes component precision:

- format 7: 5-bit Y/U/V;
- format 17: 5-bit Y/U/V with 32-symbol luma residual coding;
- format 19: 6-bit Y and literal 5-bit U/V with 64-symbol luma residual coding;
- format 20: 6-bit Y/U/V with 64-symbol luma residuals and delta-coded chroma.

These are codec properties, not user quality controls. A user-selectable colour
matrix or quantisation policy may change source conversion, but it cannot
change the bit widths of a compatible stream.

## Recommended V1 CLI Semantics

Use clear controls:

```text
--loss-level N             fixed starting level, 0..28
--target-bytes N           enable whole-frame retry around a byte target
--data-only                emit no stationary, temporal, or spatial copies
--no-temporal              disable displaced previous-frame matches
--no-spatial               disable current-frame matches
--search-radius XxY        reduce encoder search within the codec limit
```

When `--target-bytes` is absent, encode once at the requested loss level. This
is deterministic and is the right default for verification. Rate-control
retries should be opt-in until the single-pass encoder is trusted.
