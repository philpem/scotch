# The Replay colour pipeline (RGB ↔ 6Y*n*UV)

How the Moving Blocks family converts between 24-bit RGB and the working
6Y*n*UV/YUV555 samples the bitstreams carry. This is the encoder's input stage
and the player's not-quite-inverse output stage; it is referenced by every
Moving Blocks spec (types 7, 17, 19, 20). Conventions in
[methodology.md](methodology.md).

The forward transform is **CompLib's**, not a generic YUV matrix. To match Acorn
output bit-for-bit an encoder must reproduce both the exact integer constants and
the **ARM arithmetic-shift rounding** (round toward −∞), which differs from C's
truncate-toward-zero for negatives.

## 1. Per-format parameters

One transform serves all four codecs; only two range constants change:

| Codec | Working colour | `luma_max` | `chroma_max` | chroma *half* |
| ---:| --- | ---:| ---:| ---:|
| 7, 17 | YUV555 | 31 | 31 | 16 |
| 19 | 6Y5UV | 63 | 31 | 16 |
| 20 | 6Y6UV | 63 | 63 | 32 |

`luma_max` is the largest stored luma (5- or 6-bit); `chroma_max` the largest
stored chroma magnitude code. Chroma is **signed**, stored modulo `chroma_max+1`
(sign-extend with *half* = (chroma_max+1)/2; methodology §Colour).

## 2. Forward: RGB24 → working sample

For a source pixel (R, G, B), each 0..255:

### 2.1 Luma

```
fixed_y = R*19595 + G*38470 + B*7471          (16.16 fixed point)
Y       = (fixed_y * luma_max + threshold) >> 24
```

19595, 38470, 7471 are the **BT.601** luma weights (0.299, 0.587, 0.114) in 16.16
and sum to exactly 65536, so `fixed_y` is the BT.601 luma scaled by 65536 and the
quantiser maps it to 0..`luma_max`. `threshold` is the rounding bias in the
24-bit fractional domain (§3): the constant `1 << 23` (= half a step) for
plain round-to-nearest, or an ordered-dither value.

### 2.2 Chroma

U comes from B−Y and V from R−Y, using scaled partial-luma constants:

```
u_base = (R*22117 + G*43419) >> 16          v_base = (G*54878 + B*10658) >> 16
u      = asr(B - u_base, 1)                 v      = asr(R - v_base, 1)
U      = quantise_chroma(u, chroma_max)     V      = quantise_chroma(v, chroma_max)
```

where `asr(x, 1)` is an arithmetic shift right (floor(x/2), so a negative B−Y
rounds down), and

```
quantise_chroma(value, chroma_max):
    scaled = value * chroma_max
    scaled += (scaled >= 0) ? 128 : -128       # CompLib's sign-dependent half-step
    return asr(scaled, 8) & chroma_max          # floor(scaled / 256), then mask to the modulus
```

The final `& chroma_max` is what stores the signed result modulo `chroma_max+1`.
6Y6UV (`chroma_max` 63) is identical to 6Y5UV (`chroma_max` 31) but scaled by 63
rather than 31.

### 2.3 ARM-ASR rounding

Every `>>`/division above is an **arithmetic shift** (round toward −∞). In C:

```
asr(x, s):   x >= 0 ? x >> s : -((-x + (1<<s) - 1) >> s)
```

An encoder that uses C's truncate-toward-zero division will mismatch Acorn on
negative chroma.

## 3. Ordered dither (luma only)

Dither perturbs only the **luma** rounding bias `threshold`; chroma is never
dithered. The quantiser truncates `fixed_y*luma_max >> 24`, so the rounding
threshold lives in fractional bits 0..23, and `1 << 23` is the round-to-nearest
half step. An ordered-dither value `b` replaces it, scaled to span the same
24-bit range and centred on `1 << 23`:

| Mode | `threshold` |
| --- | --- |
| none (round to nearest) | `1 << 23` |
| ordered 4×4 | `bayer4x4[y & 3][x & 3] << 20` |
| ordered 8×8 | `bayer8x8[y & 7][x & 7] << 18` |

The 4×4 entries are 0..15 and their midpoint 8 gives `8 << 20 = 1 << 23`; the 8×8
entries are 0..63 with midpoint 32 giving `32 << 18 = 1 << 23` — so each spreads
the threshold ±half a quantiser step around plain rounding. The 8×8 is the
recursive doubling of the 4×4 (a finer, less visible pattern).

```
bayer4x4 =
   0  8  2 10
  12  4 14  6
   3 11  1  9
  15  7 13  5

bayer8x8 =
   0 32  8 40  2 34 10 42
  48 16 56 24 50 18 58 26
  12 44  4 36 14 46  6 38
  60 28 52 20 62 30 54 22
   3 35 11 43  1 33  9 41
  51 19 59 27 49 17 57 25
  15 47  7 39 13 45  5 37
  63 31 55 23 61 29 53 21
```

## 4. Inverse: working sample → RGB (display only)

The player turns a decoded sample back to RGB for display. **This is not the
algebraic inverse of §2** — the quantisation is lossy regardless, so this path
exists only to preview frames, never to round-trip them. It is a textbook BT.601
reconstruction:

```
luma = round(Y * 255 / luma_max)
u    = round(signed(U) * 256 / chroma_max)        # signed() sign-extends with half
v    = round(signed(V) * 256 / chroma_max)
R = clamp(luma + 1402*v / 1000)
G = clamp(luma - (344*u + 714*v) / 1000)
B = clamp(luma + 1772*u / 1000)
```

(1.402, 0.344, 0.714, 1.772 are the standard BT.601 inverse coefficients; `clamp`
is to 0..255, and the divisions round to nearest.)

## Appendix A. Provenance and corrections

Source: the ARMovie colour library
[`bas/CompLib,ffb`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/SystemRes/ARMovie/bas/CompLib%2Cffb)
(its non-dithered RGB conversion and the `6Y5UV`/`6Y6UV` scaling), embodied and
checked in the reference `mb_color.c`. The luma weights, chroma derivation and
quantiser constants are reproduced numerically; the Bayer matrices and the dither
scaling are the reference's standard ordered dither layered on CompLib's rounding.

- **It is CompLib's transform, not generic YUV.** The constants and the B−Y/R−Y
  derivation are specific to CompLib's non-dithered path; a generic full-range
  BT.601 matrix will not match.
- **Rounding is ARM-ASR throughout (§2.3).** This is the most common source of
  off-by-one chroma mismatches against Acorn output.
- **Dither is luma-only and bias-only.** Ordered dither changes only the luma
  quantiser's rounding threshold; it never touches chroma and never adds noise to
  the sample values directly.
- **Open item: exact assembler rounding at coefficient boundaries.** CompLib's
  real-to-integer rounding of its assembled constants at the very boundaries is
  reproduced by the reference but has not been confirmed from the assembler
  itself; it affects no cross-checked fixture, but a from-assembler pass would
  close it.
