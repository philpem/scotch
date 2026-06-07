#!/usr/bin/env python3
"""Run reproducible type 19 policy/quality comparisons on native frames."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from mb19_compare_reports import aggregate, format_float


def frame_path(prefix: Path, frame: int, suffix: str) -> Path:
    return Path(f"{prefix}{frame:06d}{suffix}")


def parse_size(text: str) -> tuple[int, int]:
    try:
        width_text, height_text = text.lower().split("x", 1)
        width = int(width_text)
        height = int(height_text)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("size must be WIDTHxHEIGHT") from error
    if width <= 0 or height <= 0 or width % 4 or height % 4:
        raise argparse.ArgumentTypeError(
            "width and height must be positive multiples of four"
        )
    return width, height


def parse_levels(text: str) -> list[int]:
    try:
        levels = [int(value) for value in text.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "levels must be comma-separated integers"
        ) from error
    if not levels or any(level < 0 or level > 28 for level in levels):
        raise argparse.ArgumentTypeError("levels must be in the range 0..28")
    return levels


def parse_targets(text: str) -> list[int]:
    try:
        targets = [int(value) for value in text.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "targets must be comma-separated integers"
        ) from error
    if not targets or any(target <= 0 for target in targets):
        raise argparse.ArgumentTypeError("targets must be positive")
    return targets


def run(command: list[str], *, stdout: Path | None = None) -> None:
    if stdout is None:
        subprocess.run(command, check=True)
        return
    with stdout.open("w", encoding="utf-8") as output:
        subprocess.run(command, check=True, stdout=output)


def join_sources(prefix: Path, frames: int, frame_bytes: int,
                 destination: Path) -> None:
    with destination.open("wb") as output:
        for frame in range(frames):
            source = frame_path(prefix, frame, ".6y5uv")
            data = source.read_bytes()
            if len(data) != frame_bytes:
                raise ValueError(
                    f"{source}: expected {frame_bytes} bytes, got {len(data)}"
                )
            output.write(data)


def verify_sequence(verify: Path, source_prefix: Path, payload_prefix: Path,
                    decoded_prefix: Path, report: Path, frames: int,
                    size: str) -> None:
    with report.open("w", encoding="utf-8") as output:
        for frame in range(frames):
            command = [
                str(verify), "--codec", "19",
                "--payload", str(frame_path(payload_prefix, frame, ".mb19")),
                "--size", size,
                "--output-6y5uv",
                str(frame_path(decoded_prefix, frame, ".6y5uv")),
                "--reference-6y5uv",
                str(frame_path(source_prefix, frame, ".6y5uv")),
                "--summary",
            ]
            if frame:
                command.extend([
                    "--previous-6y5uv",
                    str(frame_path(decoded_prefix, frame - 1, ".6y5uv")),
                ])
            subprocess.run(command, check=True, stdout=output)


def markdown(results: list[tuple[str, str, dict[str, int | float]]]) -> str:
    lines = [
        "| Policy | Control | Bytes | Bytes/frame | Y PSNR | U PSNR | V PSNR | Max Y error |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for policy, control, totals in results:
        lines.append(
            f"| {policy} | {control} | {totals['payload_bytes']} | "
            f"{totals['bytes_per_frame']:.3f} | "
            f"{format_float(float(totals['psnr_y']))} | "
            f"{format_float(float(totals['psnr_u']))} | "
            f"{format_float(float(totals['psnr_v']))} | "
            f"{totals['max_error_y']} |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--encode", type=Path, required=True)
    parser.add_argument("--verify", type=Path, required=True)
    parser.add_argument(
        "--source-prefix", type=Path, required=True,
        help="prefix for files named PREFIX000000.6y5uv and onward",
    )
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--size", type=parse_size, required=True)
    control = parser.add_mutually_exclusive_group()
    control.add_argument("--levels", type=parse_levels)
    control.add_argument("--targets", type=parse_targets)
    parser.add_argument(
        "--initial-level", type=int, default=7,
        help="initial and first-frame level for target-byte runs",
    )
    parser.add_argument(
        "--policies", nargs="+", choices=("lowest-error", "ordered"),
        default=("lowest-error", "ordered"),
    )
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    if args.frames <= 0:
        parser.error("--frames must be positive")
    if args.initial_level < 0 or args.initial_level > 28:
        parser.error("--initial-level must be in the range 0..28")
    width, height = args.size
    size_text = f"{width}x{height}"
    args.work_dir.mkdir(parents=True, exist_ok=True)
    joined_source = args.work_dir / "source.6y5uv"
    try:
        join_sources(
            args.source_prefix, args.frames, width * height * 3, joined_source
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    configurations = (
        [("level", value) for value in args.levels]
        if args.levels is not None
        else [("target", value) for value in args.targets]
        if args.targets is not None
        else [("level", value) for value in (0, 7, 14, 21, 28)]
    )
    results: list[tuple[str, str, dict[str, int | float]]] = []
    for policy in args.policies:
        for control_name, value in configurations:
            name = (f"{policy}-q{value:02d}" if control_name == "level"
                    else f"{policy}-target-{value}")
            run_dir = args.work_dir / name
            payload_dir = run_dir / "payload"
            decoded_dir = run_dir / "decoded"
            payload_dir.mkdir(parents=True, exist_ok=True)
            decoded_dir.mkdir(parents=True, exist_ok=True)
            payload_prefix = payload_dir / "frame-"
            decoded_prefix = decoded_dir / "frame-"
            report = run_dir / "verify.report"
            trace = run_dir / "encode.trace"

            command = [
                str(args.encode), "--codec", "19",
                "--input", str(joined_source),
                "--input-format", "6y5uv", "--size", size_text,
                "--frames", str(args.frames),
                "--payload-prefix", str(payload_prefix), "--policy", policy,
                "--trace", str(trace),
            ]
            if control_name == "level":
                command.extend(["--loss-level", str(value)])
            else:
                command.extend([
                    "--loss-level", str(args.initial_level),
                    "--target-bytes", str(value),
                ])
            run(command, stdout=run_dir / "encode.log")
            verify_sequence(
                args.verify, args.source_prefix, payload_prefix,
                decoded_prefix, report, args.frames, size_text,
            )
            results.append((policy, f"{control_name} {value}", aggregate(report)))

    output = markdown(results)
    if args.output is None:
        sys.stdout.write(output)
    else:
        args.output.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
