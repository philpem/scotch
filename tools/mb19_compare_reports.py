#!/usr/bin/env python3
"""Aggregate replay-verify --summary reports for encoder comparisons."""

from __future__ import annotations

import argparse
import math
import shlex
from pathlib import Path


COUNT_FIELDS = (
    "data4x4", "stationary4x4", "temporal4x4", "spatial4x4",
    "split4x4", "data2x2", "stationary2x2", "temporal2x2",
    "spatial2x2",
)
BIT_FIELDS = (
    "data4x4_bits", "stationary4x4_bits", "temporal4x4_bits",
    "spatial4x4_bits", "split_header_bits", "data2x2_bits",
    "stationary2x2_bits", "temporal2x2_bits", "spatial2x2_bits",
)


def fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in shlex.split(line):
        if "=" in token:
            name, value = token.split("=", 1)
            result[name] = value
    return result


def psnr(squared_error: int, samples: int, maximum: int) -> float:
    if squared_error == 0:
        return math.inf
    return 10.0 * math.log10(maximum * maximum * samples / squared_error)


def aggregate(path: Path) -> dict[str, int | float]:
    totals: dict[str, int | float] = {
        "frames": 0,
        "pixels": 0,
        "payload_bytes": 0,
        "semantic_bits": 0,
        "sse_y": 0,
        "sse_u": 0,
        "sse_v": 0,
        "max_error_y": 0,
        "max_error_u": 0,
        "max_error_v": 0,
    }
    totals.update({name: 0 for name in COUNT_FIELDS + BIT_FIELDS})

    for line in path.read_text(encoding="utf-8").splitlines():
        values = fields(line)
        if line.startswith("codec="):
            width, height = (int(value) for value in values["size"].split("x"))
            totals["frames"] += 1
            totals["pixels"] += width * height
        elif line.startswith("reference="):
            for name in ("sse_y", "sse_u", "sse_v"):
                totals[name] += int(values[name])
            for name in ("max_error_y", "max_error_u", "max_error_v"):
                totals[name] = max(int(totals[name]), int(values[name]))
        elif line.startswith("summary "):
            totals["payload_bytes"] += int(values["payload_bytes"])
            totals["semantic_bits"] += int(values["semantic_bits"])
            for name in COUNT_FIELDS:
                totals[name] += int(values[name])
            for name in BIT_FIELDS:
                totals[name] += int(values[name])

    frames = int(totals["frames"])
    pixels = int(totals["pixels"])
    if frames == 0 or pixels == 0:
        raise ValueError(f"{path}: report contains no verified frames")
    totals["bytes_per_frame"] = int(totals["payload_bytes"]) / frames
    totals["psnr_y"] = psnr(int(totals["sse_y"]), pixels, 63)
    totals["psnr_u"] = psnr(int(totals["sse_u"]), pixels, 31)
    totals["psnr_v"] = psnr(int(totals["sse_v"]), pixels, 31)
    return totals


def format_float(value: float) -> str:
    return "inf" if math.isinf(value) else f"{value:.6f}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "reports", nargs="+", metavar="LABEL=REPORT",
        help="label and replay-verify report path",
    )
    args = parser.parse_args()
    baseline_bytes: int | None = None

    for specification in args.reports:
        if "=" not in specification:
            parser.error("reports must use LABEL=REPORT")
        label, path_text = specification.split("=", 1)
        totals = aggregate(Path(path_text))
        payload_bytes = int(totals["payload_bytes"])
        if baseline_bytes is None:
            baseline_bytes = payload_bytes
        byte_delta = 100.0 * (payload_bytes - baseline_bytes) / baseline_bytes
        print(
            f"label={label} frames={totals['frames']} "
            f"payload_bytes={payload_bytes} "
            f"bytes_per_frame={totals['bytes_per_frame']:.3f} "
            f"bytes_vs_first_pct={byte_delta:.3f} "
            f"semantic_bits={totals['semantic_bits']} "
            + " ".join(f"{name}={totals[name]}" for name in COUNT_FIELDS)
            + " "
            + " ".join(f"{name}={totals[name]}" for name in BIT_FIELDS)
            + f" sse_y={totals['sse_y']} sse_u={totals['sse_u']} "
            f"sse_v={totals['sse_v']} "
            f"psnr_y={format_float(float(totals['psnr_y']))} "
            f"psnr_u={format_float(float(totals['psnr_u']))} "
            f"psnr_v={format_float(float(totals['psnr_v']))} "
            f"max_error_y={totals['max_error_y']} "
            f"max_error_u={totals['max_error_u']} "
            f"max_error_v={totals['max_error_v']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
