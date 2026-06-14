# The copy-acceptance quality model (QP%)

> **Encoder-side.** A decoder never sees any of this — it simply reconstructs
> whatever blocks the encoder chose. This spec is for building an *encoder* that
> reproduces Acorn's copy decisions, and for understanding the `loss_level` knob.

The Moving Blocks family (types 7, 17, 19, 20) decides whether a *copy* (a
stationary, temporal or spatial reference) may stand in for a block, instead of
spending bits on a fresh data block, by measuring the copy's reconstruction error
against a row of a shared 29-row "QP%" table. The chosen row is the **loss
level** (0 = exact, 28 = loosest). Whole-frame rate control retries the frame at
successively looser rows to hit a byte budget (container spec); within a frame,
one fixed row is used.

## 1. Measuring a candidate copy

For a candidate block of `area = size²` pixels (size 4 or 2), compare the
**source** block against the **reference** block (the decoded pixels the copy
would produce) and build a profile:

- **Luma, per pixel:** `d = |source.Y − reference.Y|`.
  - `max_luma_error` = the largest `d` over the block.
  - `luma_over[k]` for k = 0..6 = the **count of pixels** with `d > k`.
  - `total` accumulates `Σ d`.
- **Chroma, per block (one value each for U and V):** the block reconstructs to a
  single signed chroma pair. Compare the source block's average chroma against
  the reference block's average chroma (both signed, floored with ARM-ASR):
  `chroma_U_error = |avg(source U) − avg(reference U)|`, likewise V. Add their
  effect to the total once per pixel: `total += area * (chroma_U_error +
  chroma_V_error)`.

`total_error = total`.

## 2. Accepting against a level

Each table row is `{ maxi, maxe, total4x4, total2x2 }`:

- `maxi` (*max individual error*) — the ordinary per-component luma/chroma limit.
- `maxe` (*max exceptional error*) — a larger luma limit a few pixels may reach.
- `total4x4`, `total2x2` — the total-error ceiling for a 4×4 / 2×2 block.

A copy is **accepted** at a level when **all** of these hold (otherwise rejected):

```
exceptional_limit = (size == 4) ? 4 : 1
total_limit       = (size == 4) ? total4x4 : total2x2

max_luma_error        <= maxe                 # no pixel exceeds the exceptional luma limit
luma_over[maxi]       <= exceptional_limit     # at most this many pixels exceed maxi luma error
chroma_U_error        <= maxi
chroma_V_error        <= maxi
total_error           <= total_limit
```

In words: chroma must be within `maxi`; **at most** 4 pixels (4×4) or 1 pixel
(2×2) may have luma error above `maxi`, and **no** pixel may exceed `maxe`; and
the summed error must fit `total_limit`. Level 0 (`{0,0,0,0}`) therefore demands
an exact, decoder-visible pixel match — it is not a special code path, just the
tightest row.

The table loosens monotonically from level 0 to 28, so "the lowest level that
accepts this copy" is a binary search over the rows.

## 3. The 29-row QP% table

| Level | maxi | maxe | total 4×4 | total 2×2 | | Level | maxi | maxe | total 4×4 | total 2×2 |
| ---:| ---:| ---:| ---:| ---:|---| ---:| ---:| ---:| ---:| ---:|
| 0 | 0 | 0 | 0 | 0 | | 15 | 1 | 3 | 48 | 12 |
| 1 | 0 | 1 | 2 | 1 | | 16 | 2 | 3 | 48 | 12 |
| 2 | 0 | 1 | 4 | 2 | | 17 | 2 | 4 | 48 | 12 |
| 3 | 1 | 1 | 6 | 2 | | 18 | 2 | 4 | 72 | 18 |
| 4 | 1 | 1 | 8 | 2 | | 19 | 2 | 5 | 96 | 24 |
| 5 | 1 | 2 | 10 | 3 | | 20 | 2 | 6 | 96 | 24 |
| 6 | 1 | 2 | 12 | 3 | | 21 | 3 | 6 | 100 | 28 |
| 7 | 1 | 2 | 14 | 4 | | 22 | 3 | 7 | 132 | 33 |
| 8 | 1 | 2 | 16 | 4 | | 23 | 4 | 8 | 144 | 36 |
| 9 | 1 | 2 | 18 | 5 | | 24 | 4 | 9 | 180 | 45 |
| 10 | 1 | 2 | 20 | 5 | | 25 | 5 | 10 | 180 | 45 |
| 11 | 1 | 2 | 24 | 6 | | 26 | 5 | 11 | 195 | 48 |
| 12 | 1 | 2 | 28 | 7 | | 27 | 6 | 12 | 198 | 50 |
| 13 | 1 | 3 | 32 | 8 | | 28 | 6 | 13 | 216 | 60 |
| 14 | 1 | 3 | 36 | 9 | | | | | | |

## 4. Choosing among accepted copies (policy)

Acceptance only says a copy is *good enough*; an encoder still picks one. Two
policies produce valid (and different) streams:

- **Ordered** (the historical Acorn behaviour): take the first accepted family in
  the order stationary, temporal, spatial.
- **Lowest-error**: compare every accepted copy by reconstruction error, then by
  emitted bit cost, then by a stable family/table order.

Neither changes the bitstream *syntax* — they change which legal block an encoder
emits. When no copy is accepted, the block is coded as data (or a 2×2 split,
whichever is smaller).

## Appendix A. Provenance and corrections

Source: `PROCReadQualTable` in the Moving Blocks compressors' BASIC
(`Video/Decomp19/bas/BatchComp,ffb` and the matching `bas/BatchComp,ffb` of types
7/17/20; see [methodology.md](methodology.md) for links), embodied and verified
in the reference `mb_quality.c`. The same table is shared by all four codecs.

- **`maxe` is per pixel, `maxi` is a count threshold *plus* a chroma limit.** The
  two luma limits work together: no pixel may exceed `maxe`, and only a small
  number (`exceptional_limit`) may exceed the tighter `maxi`. `maxi` separately
  bounds the block-average chroma error. Reading either as a single global limit
  gets the acceptance wrong.
- **Chroma error is one block-average value, weighted by area.** It is not a
  per-pixel chroma difference; the block reconstructs to a single (U, V), so the
  error is measured once and counted `area` times in the total.
- **Level 0 is exact match, not a flag.** The first row's all-zero limits fall
  out of the same test as every other row.
- **The acceptance is shared; the search is not normative.** The 29 rows and the
  accept test are Acorn's; the policy in §4 (which accepted copy to emit) is an
  encoder choice that does not affect decodability.
