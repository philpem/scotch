# Quality Measurements And Tools

A running record of the signal quality the portable coders achieve, and the
tools used to measure it, so further work can compare against a baseline.

## Audio (signal-to-noise ratio, encode -> decode round trip)

SNR = 10*log10(signal_power / error_power), decoded straight from the finished
movie and compared with the signed-16 PCM it was made from. Figures below are
mono 11025 Hz unless noted; per-content results vary with material.

| Format (header)                    | bytes/sample | test signal        | SNR |
|------------------------------------|--------------|--------------------|-----|
| VIDC E8 exponential (format 1)     | 1            | 440 Hz tone        | ~36 dB |
| VIDC E8 exponential (format 1)     | 1            | real audio (clip)  | ~37 dB |
| 8-bit signed linear (SoundS8)      | 1            | -                  | ~8-bit quant. |
| 16-bit signed linear (SoundS16)    | 2            | -                  | lossless |
| IMA ADPCM mono (format 2 "adpcm")  | 0.5          | 440 Hz tone        | ~33 dB |
| IMA ADPCM stereo (Decodex2)        | 0.5/ch       | 440/880 Hz L/R     | ~28 dB |

Notes: ADPCM is ~4x smaller than 16-bit and ~half VIDC E8 for ~33 dB. Stereo
ADPCM was also cross-checked decoded-L vs source-R = -3 dB, confirming the two
channels are not swapped. VIDC E8 is the nearest-match inverse of Acorn's exact
`ELogToLinTable`, so a round trip reproduces the player's decode bit-for-bit.

Measured with `tools/replay_audio_snr.py MOVIE,ae7 --reference ref.s16le`
(make the reference with `ffmpeg -i src -vn -ac N -ar R -f s16le ref.s16le`).
That tool decodes the movie's sound track per chunk (E8, signed 8/16, ADPCM
mono/stereo) the same way the player does, and can also dump the decoded PCM
with `--output`.

## Video (type 19 luma/chroma PSNR vs the source 6Y5UV)

The authoritative figures live in `docs/implementation-status.md` (Verified
Claims) and `notes/moving-blocks-decision-comparison.md`; summary:

- 25 authoritative type-2 frames at quality 7: portable luma PSNR 45.24 dB
  (lowest-error policy) vs Acorn's 45.22 dB, with maximum luma error 2; the
  legacy "ordered" policy is 42.77 dB. U/V are within ~0.2-0.5 dB of Acorn.
- Sequence PSNR (from summed squared error, not averaged per-frame) over
  fixed-level and matched-target sweeps is produced by
  `tools/mb19_quality_sweep.py` (see `docs/encoder-policy-comparison.md`).
- loss-level vs size on an 8 s 320x180 clip: loss 4 ~1.02 MB, 8 ~0.93 MB,
  12 ~0.89 MB, 16 ~0.78 MB; the size is nearly flat, so a lower loss level buys
  cleaner output for little cost.

### Cross-codec rate/quality (PSNR vs RGB)

To compare codecs with different working colour spaces (type 19 6Y5UV vs type
17 YUV555) on an even footing, `tools/replay_quality_curve.py` measures PSNR
back in RGB: it encodes the same frames with `replay-encode --recon-prefix` and
compares each codec's RGB reconstruction preview to the original RGB. It sweeps
loss levels per codec and reports PSNR, total bytes and bytes/frame (CSV +
optional PSNR-vs-bytes/frame chart). PSNR is the global value from summed
squared error, not an average of per-frame PSNRs.

Important: the 29-row QP table is shared by the whole Moving Blocks family in
*absolute* luma-error units, but type 19 luma is 6-bit and type 17 luma is
5-bit. The same loss level therefore allows the same absolute luma error over a
half-as-wide range, so type 17 at a given loss level is roughly twice as lossy
(and smaller) as type 19 at the same level. Compare codecs at equal PSNR, not
equal loss level.

Measured rate/quality, Digital Circus 160x88, 30 frames (PSNR vs RGB):

| loss | type 17 PSNR / B-frame | type 19 PSNR / B-frame |
|-----:|------------------------|------------------------|
| 0    | 28.09 / 4710           | 28.18 / 5895           |
| 4    | 28.09 / 3652           | 28.31 / 4996           |
| 8    | 27.79 / 3041           | 28.40 / 4437           |
| 12   | 27.22 / 2672           | 28.20 / 4145           |
| 16   | 25.64 / 1903           | 28.00 / 3403           |
| 20   | 23.80 / 1420           | 27.11 / 2830           |

Reading: at equal *loss level* type 17 is smaller but lower PSNR -- the earlier
"32% smaller" was a quality illusion from comparing at equal loss, which the
eye correctly caught. At equal *PSNR* the two are close (e.g. ~27.2 dB: type 17
2672 vs type 19 2830 B/frame), but type 19 has the higher ceiling (~28.4 dB at
loss 8) that type 17 cannot reach (it tops out ~28.1 dB at loss 0, its 5-bit
luma limit) and its PSNR falls off much faster as loss rises. To make a type 17
cut that looks like the type 19 loss-10 cut, use a much lower type 17 loss
level (~0-4), not the same number.

Applied mitigations (now default for type 17 via replay-make):
- Ordered 8x8 Bayer *luma* dither in RGB->YUV (mb_color, --dither 8x8). Confirmed
  visually to clear most of the smooth-gradient/light-fade luma banding. Costs
  ~0.35 dB PSNR and ~4% size; 8x8 reads cleaner than 4x4 at the same cost.
- Lowest-error copy policy (default for type 17, --policy). +0.12 dB at equal
  size without dither; with dither it shifts a few blocks to data (~+4-5% size).
  Helps the high-frequency edge shimmer.

Future work (parked):
- CHROMA block banding (confirmed 2026-06-13): vertical/horizontal bands on red
  and brown objects with a saturation gradient. This is the codecs' one
  block-averaged 5-bit U/V per 4x4 (or 2x2) stepping along the block grid -- the
  same in type 19. Luma dither only helps it marginally. The fix would be an
  ordered dither on the *per-block* chroma rounding in mb_encode_average_chroma
  (perturb adjacent blocks' U/V differently). Parked: judged a rabbit hole for
  the gain. A synthetic `gradients`/`testsrc2` source (via `--lavfi`) stresses
  the luma case; a saturation ramp stresses the chroma case.
- Frozen luma banding in near-static gradients: stationary/temporal copies hold
  the quantisation in place so the eye locks on. Dither largely masks it now; a
  periodic data-block refresh of long-lived copy regions would also help.

## Measurement tools

- `tools/replay_quality_curve.py` - PSNR-vs-size rate/quality curves for any
  codec, measured in RGB so different working colour spaces compare fairly.
  Reusable for new codecs as they are added.
- `tools/replay_audio_snr.py` - decode a movie's sound track and measure SNR
  against a reference, or extract it to PCM. Reusable for any movie this
  toolchain makes.
- `mb_metrics` / `replay-verify --reference-6y5uv` - native 6Y5UV SSE/MSE, PSNR
  and maximum-error metrics per frame, in the encoder traces and the verifier.
- `tools/mb19_quality_sweep.py` - reproducible fixed-level and matched-target
  PSNR/bitrate sweeps over numbered native frames, preserving every payload,
  reconstruction, trace and report.
- `tools/decomp19_unicorn.py` - decode a payload (or a movie chunk) through the
  genuine compiled Acorn Decomp19 under Unicorn, for byte-exact cross-checks.
- `tools/mb19_compare_reports.py` - aggregate verifier reports without
  incorrectly averaging per-frame PSNR values.
