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

## Measurement tools

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
