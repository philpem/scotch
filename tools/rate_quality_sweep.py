#!/usr/bin/env python3
"""PSNR-vs-bitrate rate/quality sweep across the Moving Blocks codec family.

This is a thin driver around tools/replay_quality_curve.py that adds two things
the bare tool does not do:

  * per-*series* extra encode args, so the two type-20 (Moving Blocks Beta)
    variants -- old (Sep'96, direct 6-bit chroma) and new (Nov'96, delta chroma,
    container type-aliased to 30) -- can sit on the same chart; and
  * an x-axis expressed as real bitrate (kbit/s at the playback frame rate)
    instead of bytes/frame.

Method (same as replay_quality_curve.py): ffmpeg decodes the chosen segment to
packed RGB24 at the target size and frame rate; for every (series, loss-level)
point replay-encode encodes those exact frames and writes an RGB reconstruction
preview via --recon-prefix; PSNR is the global (whole-segment) RGB MSE turned
into dB. Measuring back in RGB keeps codecs with different internal colour
spaces on an even footing. Bitrate is sum(payload bytes)/frames * fps * 8.

Sampling stays at the playback frame rate (default 12.5 fps) and the segment is
contiguous, so the motion-compensated codecs see realistic inter-frame
coherence -- sparsely sampling across the whole runtime would unfairly penalise
them. To cover varied content, lengthen the segment (--frames), don't drop fps.

Example:
  tools/rate_quality_sweep.py \
      --input input-videos/clip.webm --start 0 --frames 5400 \
      --size 160x88 --fps 12.5 --out-prefix docs/quality/rate_quality_long
"""

import argparse
import csv
import glob
import importlib.util
import math
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

# Load the sibling tool for its ffmpeg extract + PPM reader (single source of truth).
_QC_PATH = os.path.join(os.path.dirname(__file__), "replay_quality_curve.py")
_spec = importlib.util.spec_from_file_location("replay_quality_curve", _QC_PATH)
qc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qc)

# label, codec number, extra replay-encode args for this series
SERIES = [
    ("7 — Moving Blocks", 7, []),
    ("17 — Moving Blocks HQ", 17, []),
    ("19 — Super Moving Blocks", 19, []),
    ("20 — Beta (old, Sep'96)", 20, ["--variant", "old"]),
    ("20/30 — Beta (new, Nov'96)", 20, ["--variant", "new"]),
]


def measure(encode_bin, work, codec, loss, src, w, h, frames, extra):
    """Encode one point and return (psnr_db, total_payload_bytes)."""
    for stale in glob.glob(f"{work}/r*.ppm") + glob.glob(f"{work}/f*"):
        os.remove(stale)
    cmd = [encode_bin, "--codec", str(codec), "--input", f"{work}/src.rgb",
           "--size", f"{w}x{h}", "--input-format", "rgb24",
           "--payload-prefix", f"{work}/f", "--recon-prefix", f"{work}/r",
           "--frames", str(frames), "--loss-level", str(loss),
           "--no-dither", "--policy", "lowest-error"] + extra
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    recons = sorted(glob.glob(f"{work}/r*.ppm"))
    payloads = sorted(glob.glob(f"{work}/f*"))
    total = sum(os.path.getsize(p) for p in payloads)
    se, n = 0.0, 0
    for i, rp in enumerate(recons):
        recon = qc.read_ppm(rp, w, h)
        se += float(np.sum((src[i] - recon) ** 2))
        n += recon.size
    mse = se / n if n else 0.0
    psnr = 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return psnr, total


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", required=True, help="source video (decoded with ffmpeg)")
    p.add_argument("--size", required=True, help="WIDTHxHEIGHT (multiples of 4)")
    p.add_argument("--frames", type=int, required=True)
    p.add_argument("--fps", default="12.5", help="sampling + playback rate")
    p.add_argument("--start", default="0", help="start offset (seconds)")
    p.add_argument("--loss-levels", default="0,4,8,12,16,20,24,28")
    p.add_argument("--encode-bin", default="./build-release/replay-encode")
    p.add_argument("--ffmpeg", default="ffmpeg")
    p.add_argument("--out-prefix", required=True,
                   help="writes <prefix>.csv and <prefix>_bitrate.png")
    p.add_argument("--title", default="", help="extra line for the chart title")
    args = p.parse_args()

    w, h = (int(v) for v in args.size.lower().split("x", 1))
    losses = [int(v) for v in args.loss_levels.split(",")]
    fps = float(args.fps)

    work = tempfile.mkdtemp(prefix="rq-sweep-")
    try:
        qc.extract_rgb(args.ffmpeg, args.input, None, w, h, args.frames,
                       args.fps, args.start, f"{work}/src.rgb")
        need = args.frames * h * w * 3
        raw = np.fromfile(f"{work}/src.rgb", dtype=np.uint8)
        if raw.size < need:
            sys.exit(f"source decoded to {raw.size} bytes; need {need} for "
                     f"{args.frames} {w}x{h} frames (segment shorter than asked?)")
        src = raw[:need].reshape(args.frames, h, w, 3).astype(np.float64)

        results = {}
        print(f"{'series':30} {'loss':>4} {'PSNR':>7} {'B/frame':>9} {'kbit/s':>8}")
        for label, codec, extra in SERIES:
            pts = []
            for loss in losses:
                psnr, total = measure(args.encode_bin, work, codec, loss, src,
                                      w, h, args.frames, extra)
                bpf = total / args.frames
                pts.append((bpf, psnr, loss, total))
                print(f"{label:30} {loss:4d} {psnr:7.2f} {bpf:9.1f} "
                      f"{bpf * fps * 8 / 1000:8.1f}", flush=True)
            results[label] = pts
    finally:
        shutil.rmtree(work, ignore_errors=True)

    csv_path = f"{args.out_prefix}.csv"
    with open(csv_path, "w", newline="") as fh:
        wri = csv.writer(fh)
        wri.writerow(["series", "loss_level", "psnr_db", "total_bytes",
                      "bytes_per_frame", "kbit_s"])
        for label, pts in results.items():
            for bpf, psnr, loss, total in pts:
                wri.writerow([label, loss, f"{psnr:.4f}", total, f"{bpf:.2f}",
                              f"{bpf * fps * 8 / 1000:.2f}"])

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.figure(figsize=(8.5, 5.5))
    for label, pts in results.items():
        pp = sorted(pts)
        style = dict(marker="s", linestyle="--") if "new" in label else dict(marker="o")
        plt.plot([x[0] * fps * 8 / 1000 for x in pp], [x[1] for x in pp],
                 label=label, **style)
    plt.xlabel(f"bitrate (kbit/s @ {args.fps} fps)")
    plt.ylabel("PSNR (dB, measured in RGB)")
    title = "Acorn Replay Moving Blocks codecs — rate / quality"
    if args.title:
        title += "\n" + args.title
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend(title="codec / variant")
    plt.tight_layout()
    png_path = f"{args.out_prefix}_bitrate.png"
    plt.savefig(png_path, dpi=130)
    print(f"wrote {csv_path} and {png_path}")


if __name__ == "__main__":
    main()
