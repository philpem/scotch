#!/usr/bin/env python3
"""Measure PSNR-vs-size rate/quality curves for Replay video codecs.

For each codec and loss level the script encodes the same source frames with
replay-encode (writing RGB reconstruction previews via --recon-prefix), then
compares those reconstructions against the original RGB to compute PSNR. This
lets new codecs be compared on an even footing: quality is always measured back
in RGB, regardless of each codec's internal working colour space.

Examples
--------
  # From a video file (needs ffmpeg), 60 frames at 320x180, default loss sweep:
  tools/replay_quality_curve.py --input clip.webm --size 320x180 --frames 60 \
      --codecs 17,19 --csv curve.csv --plot curve.png

  # From pre-extracted packed RGB24 frames (no ffmpeg needed):
  tools/replay_quality_curve.py --rgb frames.rgb --size 320x180 --frames 60

The result is printed as a table, optionally written as CSV, and optionally
plotted (PSNR vs bytes/frame) when matplotlib is available.
"""

import argparse
import glob
import math
import os
import subprocess
import sys
import tempfile


def parse_size(text):
    width, height = (int(v) for v in text.lower().split("x", 1))
    return width, height


def extract_rgb(ffmpeg, video, lavfi, width, height, frames, fps, start,
                out_path):
    vf = f"fps={fps},scale={width}:{height}:flags=lanczos"
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if lavfi:
        # A synthetic source already carries its own size/rate; e.g.
        #   "testsrc2=size=160x88:rate=12.5" or "gradients=size=160x88".
        cmd += ["-f", "lavfi", "-t", str(max(1, frames) / float(fps)),
                "-i", lavfi]
    else:
        if start:
            cmd += ["-ss", str(start)]
        cmd += ["-i", video]
    cmd += ["-vf", vf, "-frames:v", str(frames), "-pix_fmt", "rgb24",
            "-f", "rawvideo", out_path]
    subprocess.run(cmd, check=True)


def read_ppm(path, width, height):
    import numpy as np
    data = open(path, "rb").read()
    # P6 header: "P6\n<w> <h>\n<maxval>\n" (maxval assumed 255).
    offset = data.index(b"255\n") + 4
    pixels = np.frombuffer(data[offset:], dtype=np.uint8)
    return pixels.reshape(height, width, 3).astype(np.float64)


def measure(encode_bin, work, codec, loss, src, width, height, frames, dither, policy):
    """Encode one (codec, loss) point and return (psnr_db, total_bytes)."""
    import numpy as np
    for stale in glob.glob(f"{work}/r*.ppm") + glob.glob(f"{work}/f*"):
        os.remove(stale)
    cmd = [encode_bin, "--codec", str(codec), "--input", f"{work}/src.rgb",
           "--size", f"{width}x{height}", "--input-format", "rgb24",
           "--payload-prefix", f"{work}/f", "--recon-prefix", f"{work}/r",
           "--frames", str(frames), "--loss-level", str(loss)]
    cmd += ["--no-dither"] if dither == "none" else ["--dither", dither]
    cmd += ["--policy", policy]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    recons = sorted(glob.glob(f"{work}/r*.ppm"))
    payloads = sorted(glob.glob(f"{work}/f*"))
    total_bytes = sum(os.path.getsize(p) for p in payloads)
    # Global MSE across every reconstructed frame (standard video PSNR).
    squared_error = 0.0
    count = 0
    for i, recon_path in enumerate(recons):
        recon = read_ppm(recon_path, width, height)
        squared_error += float(np.sum((src[i] - recon) ** 2))
        count += recon.size
    mse = squared_error / count if count else 0.0
    psnr = 99.0 if mse == 0.0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return psnr, total_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", help="source video (decoded with ffmpeg)")
    source.add_argument("--lavfi", help="synthetic ffmpeg source, e.g. "
                        "'testsrc2=size=160x88:rate=12.5' or 'gradients=size=160x88'")
    source.add_argument("--rgb", help="pre-extracted packed RGB24 frames")
    parser.add_argument("--size", required=True, help="WIDTHxHEIGHT")
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--fps", default="12.5", help="sampling rate for --input")
    parser.add_argument("--start", default="0", help="start offset for --input")
    parser.add_argument("--codecs", default="17,19",
                        help="comma-separated codec numbers")
    parser.add_argument("--loss-levels", default="0,4,8,12,16,20,24,28",
                        help="comma-separated loss levels")
    parser.add_argument("--dither", default="none",
                        choices=("none", "4x4", "8x8"),
                        help="luma dither mode passed to replay-encode")
    parser.add_argument("--policy", default="lowest-error",
                        choices=("ordered", "lowest-error"),
                        help="copy selection policy passed to replay-encode")
    parser.add_argument("--encode-bin", default="./build-release/replay-encode")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--csv", help="write results as CSV to this path")
    parser.add_argument("--plot", help="write a PSNR-vs-bytes/frame chart (PNG)")
    args = parser.parse_args()

    try:
        import numpy  # noqa: F401
    except ImportError:
        sys.exit("replay_quality_curve: numpy is required")

    width, height = parse_size(args.size)
    codecs = [int(c) for c in args.codecs.split(",")]
    losses = [int(v) for v in args.loss_levels.split(",")]

    work = tempfile.mkdtemp(prefix="replay-quality-")
    try:
        if args.input or args.lavfi:
            extract_rgb(args.ffmpeg, args.input, args.lavfi, width, height,
                        args.frames, args.fps, args.start, f"{work}/src.rgb")
        else:
            import shutil
            shutil.copyfile(args.rgb, f"{work}/src.rgb")

        import numpy as np
        expected = args.frames * height * width * 3
        raw = np.fromfile(f"{work}/src.rgb", dtype=np.uint8)
        if raw.size < expected:
            sys.exit(f"source has {raw.size} bytes; need {expected} for "
                     f"{args.frames} {width}x{height} frames")
        src = raw[:expected].reshape(args.frames, height, width, 3).astype(np.float64)

        rows = []
        header = f"{'codec':>5} {'loss':>4} {'PSNR(dB)':>9} {'bytes':>9} {'B/frame':>8}"
        print(header)
        print("-" * len(header))
        for codec in codecs:
            for loss in losses:
                psnr, total = measure(args.encode_bin, work, codec, loss, src,
                                      width, height, args.frames,
                                      args.dither, args.policy)
                per_frame = total / args.frames
                rows.append((codec, loss, psnr, total, per_frame))
                print(f"{codec:5d} {loss:4d} {psnr:9.2f} {total:9d} {per_frame:8.1f}",
                      flush=True)
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)

    if args.csv:
        with open(args.csv, "w") as handle:
            handle.write("codec,loss_level,psnr_db,total_bytes,bytes_per_frame\n")
            for codec, loss, psnr, total, per_frame in rows:
                handle.write(f"{codec},{loss},{psnr:.4f},{total},{per_frame:.2f}\n")
        print(f"wrote {args.csv}")

    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("matplotlib not available; skipping --plot", file=sys.stderr)
            return
        plt.figure(figsize=(7, 5))
        for codec in codecs:
            pts = sorted((r[4], r[2]) for r in rows if r[0] == codec)
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            plt.plot(xs, ys, marker="o", label=f"codec {codec}")
        plt.xlabel("bytes / frame")
        plt.ylabel("PSNR (dB)")
        plt.title("Replay codec rate/quality")
        plt.grid(True, alpha=0.3)
        plt.legend()
        plt.tight_layout()
        plt.savefig(args.plot, dpi=120)
        print(f"wrote {args.plot}")


if __name__ == "__main__":
    main()
