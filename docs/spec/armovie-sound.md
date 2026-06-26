# ARMovie sound formats

Authoritative specification of the sound carried in an ARMovie/AE7 file. Read the
[container spec](ae7-armovie-container.md) for where sound lives (per-chunk
regions after each chunk's video) and [methodology.md](methodology.md) for the
conventions. Sound is independent of the video codec — any video type may carry
any sound format, or none.

## 1. How sound is described and laid out

The AE7 header (container spec §2) describes the sound with four fields:

- **Sound format** (line 10) — the codec id: **1** = the VIDC/linear family
  (§2), **2** = IMA/DVI ADPCM (§3), **0** = no sound.
- **Sound rate** (line 11) — sample rate in Hz. Some writers instead give the
  sample *period* in microseconds, labelled `µs samples` (e.g. `72 µs samples` =
  1 000 000 / 72 ≈ 13 889 Hz); a reader keys on the unit and converts to Hz. See
  [the container spec §2.3](ae7-armovie-container.md).
- **Sound channels** (line 12) — 1 (mono) or 2 (stereo).
- **Sound precision** (line 13) — bits per sample, written as a number followed
  by descriptive text that *also selects the decoder* within format 1 (§2). A
  silent movie may leave lines 11–13 blank; treat a blank numeric field as 0.

Samples are **interleaved** for stereo (L, R, L, R, …). Each chunk's sound
follows that chunk's video (container §4); the catalogue's per-track byte counts
give each track's size. For seekable per-chunk decoding, the stateful formats
prepend a small state header to each chunk's region (§3.3).

## 2. Format 1 — VIDC / linear

Format 1 covers three encodings that share the header field; the player picks
which by inspecting the **precision text**: if it contains `LIN` the samples are
signed *linear*, otherwise they are VIDC *exponential*. The bit count (8 or 16)
and channel interleave then fully determine the layout.

| Encoding | Precision field | Bytes/sample | Payload |
| --- | --- | --- | --- |
| VIDC exponential 8-bit | `8 bits per sample (exponential)` | 1 | one log code per sample (§2.1) |
| Signed linear 8-bit | `8 bit linear signed` | 1 | the high byte of the signed 16-bit sample |
| Signed linear 16-bit | `16 bit linear signed` | 2 | signed 16-bit, **little-endian** |

(So an 8-bit linear sample is the top 8 bits of a 16-bit signed value; the low
byte is discarded.)

### 2.1 VIDC 8-bit "exponential" (E format)

A sign-magnitude **logarithmic** companding: each 8-bit code maps to a signed
16-bit sample through Acorn's `ELogToLinTable`. Even codes are non-negative, the
next odd code is the matching negative magnitude, and the step size doubles up
the curve (a piecewise-exponential µ-law-like law). Decoding is a 256-entry table
lookup; encoding picks the nearest code in the matching sign family.

The authoritative table is Acorn's
[`Audio/SoundFile/s/e8to16`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Audio/SoundFile/s/e8to16)
(`ELogToLinTable`). Its shape, with `e[code]` the reconstructed sample:

```
e[0]=0      e[1]=0       e[2]=8     e[3]=-8     e[4]=16   e[5]=-16   ...
e[32]=128   e[33]=-128   e[34]=144  ...                  (step 16)
...                                                       (steps double up the curve)
e[252]=30592  e[253]=-30592  e[254]=31616  e[255]=-31616
```

The full 256 entries are in the linked source (and reproduced, verified
round-trip-exact, in the reference `replay_sound.c`).

## 3. Format 2 — IMA / DVI ADPCM

Format 2 is the canonical IMA/DVI ADPCM (the Jack Jansen 7 Jul 1992 reference
implementation Acorn adopted): each sample is a **4-bit code** predicting the
next sample from the previous one plus an adaptive step.

### 3.1 Decode

State is `(predicted, step_index)`, starting from the chunk header (§3.3). For
each 4-bit `code` (bit 3 = sign, bits 0–2 = magnitude):

```
step  = step_table[step_index]
diff  = step >> 3
if (code & 4) diff += step
if (code & 2) diff += step >> 1
if (code & 1) diff += step >> 2
predicted += (code & 8) ? -diff : diff
clamp predicted to [-32768, 32767]
step_index += index_table[code]
clamp step_index to [0, 88]
output = predicted
```

`index_table` (16 entries) and `step_table` (89 entries) are part of the format:

```
index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 }

step_table[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
  45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
  230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2747, 3022,
  3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
  29794, 32767 }
```

### 3.2 Nibble packing

Two 4-bit codes per byte, **first sample in the low nibble**.

- **Mono:** code for sample *i* is the low nibble of byte `i/2` when *i* is even,
  the high nibble when odd.
- **Stereo:** one byte per stereo frame — the **left** code in the low nibble and
  the **right** code in the high nibble.

### 3.3 Per-chunk state header

Because ADPCM is stateful, each chunk's sound region begins with a **4-byte state
header per channel**, holding the coder state at that chunk's first sample so a
chunk can be decoded after a seek without replaying earlier chunks:

```
offset 0: predicted (valprev), signed 16-bit little-endian
offset 2: step_index (0..88), one byte
offset 3: one pad byte (0)
```

For stereo, the left channel's header precedes the right channel's, then the
interleaved code bytes follow.

## 4. Format 101 — Eidos "Escape" (WINSTR ADPCM)

Sound format **101** is **not** an Acorn format: the stock ARMovie player's
`ToUseSound` only dispatches formats 1 and 2, so a 101 track plays silently there.
It is the marker the Eidos **Escape** movies use (video formats 122/130), decoded
by the built-in sound routine of Eidos' Windows "Streamer" engine, `WINSTR.DLL`.
The header labels it `"sound format - standard"` with a `"LINEAR UNSIGNED"`
precision comment, but for **4-bit** precision that label is misleading — the data
is **ADPCM**, not linear PCM. (WINSTR's dispatcher routes 4-bit precision to its
ADPCM decoder and other depths to a linear-PCM copy.)

The 4-bit codec is an IMA/DVI-style ADPCM with **two deliberate deviations** from
canonical IMA (both verified in WINSTR.DLL, ImageBase `0x10000000`, decoder at RVA
`0x10004840`):

1. The magnitude reconstruction **omits the `step >> 3` bias**:

   ```
   diff = ((code & 7) * step) >> 2        (WINSTR)
   diff = ((2*(code & 7) + 1) * step) >> 3 (canonical IMA)
   ```

2. **`step_table[62] = 2749`** (canonical 2747) and **`step_table[63] = 3024`**
   (canonical 3022). The index table is canonical IMA.

Decoding with stock IMA maths instead makes the audio drift audibly. Framing:

- **Two codes per byte, high nibble first** (the earlier sample is the high
  nibble). No `step >> 3`; `index += index_table[code]`, clamped 0..88.
- **One running state for the whole movie.** Unlike format 2 there is **no
  per-chunk state header** and the state is **never reset** at chunk boundaries —
  initialise `{predicted=0, index=0}` once and feed every chunk's code bytes in
  order. (A decoder therefore concatenates all chunks' sound regions.)
- **Stereo** splits each byte: high nibble → left, low nibble → right, with one
  running state per channel.

Reference vector (mono): the bytes `88 44 77 00` decode to
`0 0 7 16 35 75 75 75`. The reference decoder is
[`src/replay_escape_adpcm.c`](../../src/replay_escape_adpcm.c)
(`include/replay/replay_escape_adpcm.h`), driven for format 101 by
`replay-transcode`; validated bit-for-bit against the standalone
`escape_adpcm` reconstruction on `Victory.rpl` (mono) and `inflight.rpl` (stereo).

> The 8-bit (linear-PCM) and 16-bit format-101 paths are unconfirmed — no sample
> with a transcoder-supported video codec exists (the one 8-bit sample, `tank`,
> is Escape 124 video, which the transcoder does not decode).

## Appendix A. Provenance and corrections

Sources (RISC OS 2003 tree; see [methodology.md](methodology.md) for the link
convention): the VIDC exponential table from
[`Audio/SoundFile/s/e8to16`](https://github.com/barryc-ro/RiscOS_2003/blob/master/RiscOS/Sources/Audio/SoundFile/s/e8to16)
(`ELogToLinTable`); the ARMovie sound handling and the precision-text decoder
selection from the player engine
(`RiscOS/Sources/SystemRes/ARMovie`, `ToUseSound`); and the IMA/DVI ADPCM tables
and step law from the 7 Jul 1992 reference codec. The reference `replay_sound.c`
embodies these and round-trips byte-exact against the table.

- **The precision *text*, not just the number, selects the decoder.** Two
  different format-1 encodings share "8 bits": `8 bit linear signed` and
  `8 bits per sample (exponential)`. A player keys on whether the text contains
  `LIN`. A muxer must write the matching label, and a demuxer must read it — the
  number alone is ambiguous.
- **8-bit linear is the high byte.** Signed-8 stores the top 8 bits of the 16-bit
  sample; the low byte is dropped (not rounded).
- **ADPCM needs the per-chunk header to be seekable.** Without the 4-byte
  per-channel state header at each chunk boundary, a chunk cannot be decoded
  independently; the player relies on it on the streaming path.
- **A format-1 ADPCM variant exists.** Besides format 2 ("adpcm", the named
  SoundDir decompressor), Acorn also shipped a format-1 built-in `SoundA4`
  ADPCM (the tooling calls it `adpcm-sounda4`). It is a distinct carrier of the
  same ADPCM idea; its exact framing is not specified here and is a known gap.
