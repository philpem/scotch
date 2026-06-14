# Codec rate / quality measurement

PSNR-vs-bitrate comparisons of the Moving Blocks codec family (types 7, 17, 19,
20). This directory holds the generated charts and CSVs plus the method used to
produce them.

## What is measured

For each **series** (a codec, plus a variant where it has one) and each
**loss level**, we encode the same source frames and compute the picture
quality of the decoder's reconstruction:

| Series                       | Codec | Notes |
|------------------------------|-------|-------|
| Moving Blocks                | 7     | literal data, ±4 motion |
| Moving Blocks HQ             | 17    | |
| Super Moving Blocks          | 19    | |
| Beta (old, Sep'96)           | 20    | direct 6-bit chroma — `replay-encode`'s **default** for `--codec 20` |
| Beta (new, Nov'96)           | 20    | delta-coded chroma; container type-aliased to **30**. `--variant new` |

- **Quality = PSNR measured back in RGB.** `replay-encode --recon-prefix` writes
  each reconstructed frame as an RGB PPM (exactly what the decoder would hand the
  display). PSNR is the global MSE over *every pixel of every frame* in the
  segment, turned into dB (`10·log10(255²/MSE)`). Measuring in RGB — not in each
  codec's internal YUV — keeps codecs with different colour spaces comparable.
- **Bitrate = encoded payload size.** We sum the per-frame payload bytes
  (`--payload-prefix`) and report both `bytes/frame` and
  `kbit/s = bytes/frame × fps × 8 / 1000` at the playback frame rate. These are
  raw codec payloads — no container/chunk overhead — so the type-20 alias to 30
  does not affect the numbers (only `--variant` changes the bitstream).

## Fixed encode settings

All points share these so only codec + loss + rate vary:

- `--no-dither` (luma dithering off — it trades PSNR for perceived quality and
  would muddy the comparison)
- `--policy lowest-error` (copy-block selection minimises error)
- loss levels `0,4,8,12,16,20,24,28` (the full `0..28` range, every 4th)
- source decoded by ffmpeg to packed RGB24 at the target size with
  `scale=...:flags=lanczos`

## Sampling: contiguous, at the playback rate

Frames are sampled **contiguously at the playback frame rate (12.5 fps)**, not
sparsely across the runtime. The motion-compensated codecs rely on inter-frame
coherence; jumping frames would inject artificial motion and unfairly penalise
them. To cover more varied content (e.g. different lighting), **lengthen the
segment** (more `--frames`) rather than lowering the sampling rate. Encoding the
whole clip at 12.5 fps gives full-length coverage with realistic playback-rate
motion.

## Tools

- `tools/replay_quality_curve.py` — the original single-call tool (codecs as a
  flat list, no per-series variants; plots bytes/frame).
- `tools/rate_quality_sweep.py` — thin driver used for these results. Adds
  per-series extra encode args (so both type-20 variants share a chart) and a
  true kbit/s x-axis. Reuses `replay_quality_curve.py`'s ffmpeg extract and PPM
  reader so the measurement path is identical.

`rate_quality_sweep.py` defaults `--encode-bin` to `./build-release/replay-encode`
(an optimised release build — encoding is far slower without `-O`); pass
`--encode-bin` to point at another build. Both tools need `numpy`, `matplotlib`,
and `ffmpeg`.

## Reproducing the runs

Sources live in `../../input-videos/` (not in the repo). Each run writes
`<prefix>.csv` and `<prefix>_bitrate.png`.

```sh
# Source 1 — animation, 160×88, short segment (48 frames from 15 s)
tools/rate_quality_sweep.py \
  --input ../input-videos/Stupendium_MerryGoRound_DigitalCircus.webm \
  --start 15 --frames 48 --size 160x88 --fps 12.5 \
  --out-prefix docs/quality/rate_quality

# Source 2 — live action, 160×120, short segment (48 frames from 20 s)
tools/rate_quality_sweep.py \
  --input ../input-videos/Scotch_ReRecordNotFadeAway_Nov1987.mkv \
  --start 20 --frames 48 --size 160x120 --fps 12.5 \
  --out-prefix docs/quality/rate_quality_src2

# Source 1 — full clip (~7m16s ≈ 5400 frames @ 12.5 fps), wide lighting coverage
tools/rate_quality_sweep.py \
  --input ../input-videos/Stupendium_MerryGoRound_DigitalCircus.webm \
  --start 0 --frames 5400 --size 160x88 --fps 12.5 \
  --out-prefix docs/quality/rate_quality_long
```

## Findings

Measured on three runs: two short segments (animation 160×88 and live-action
160×120, 48 frames each) and the **full animation clip** (5400 frames ≈ 7m16s,
160×88) for full-length, varied-lighting coverage.

- **Beta (type 20) leads on every run.** The margin is content- and
  length-dependent: ~2–3 dB on the short animation segment and ~4–5 dB on the
  short live-action segment, but only **~1–1.5 dB over the full animation clip**
  (Beta old ~26.0–26.3 dB vs codec 7 ~25.4 and 17/19 ~24.9–25.2). Short clips
  overstate the lead; averaged over the whole video it is real but smaller.
- **Beta old (Sep'96) ≥ Beta new (Nov'96)** at every matched loss level on all
  three runs, by ~0.1–0.4 dB (full clip: old 26.05–26.30 vs new 25.73–26.07).
  This is the most stable result — the new delta-chroma rev is a
  coding-efficiency change, essentially fidelity-neutral, not a quality
  improvement.
- Every codec shows a **backward bend** at the lowest loss level: loss 0 spends
  more bytes yet scores *lower* PSNR than loss 8–16. The bend is milder over the
  full clip but still present; the useful operating range is roughly loss 4–20.
- Rankings *among* 7/17/19 shuffle with content (codec 7 leads the mid-range on
  the full clip); only **Beta's lead and the old-vs-new ordering are stable**
  across all three runs.

## Caveats

- PSNR ranks fidelity, not perceived quality; dithering (off here) and chroma
  handling can shift subjective preference away from the PSNR winner.
- Absolute PSNR and the size of Beta's lead depend on clip content and length —
  short segments exaggerate both. The full-clip run is the more representative
  figure; per-codec *ordering* nonetheless held across every run.
