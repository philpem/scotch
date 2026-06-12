#!/usr/bin/env python3
"""Run selected Acorn generated video decompressors under Unicorn.

A compiled binary is bundled under !ARMovie_compiled/Decomp19. Another binary
with the same CodecIf contract may be supplied explicitly. The harness leaves
the colour-lookup patch table untouched so each output word remains packed as
the codec's native 6Y5UV value: Y in bits 0..5, U in 6..10, V in 11..15.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

try:
    from unicorn import (
        Uc, UcError, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, UC_PROT_ALL
    )
    from unicorn.arm_const import (
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_R3,
        UC_ARM_REG_R4,
        UC_ARM_REG_R5,
        UC_ARM_REG_R6,
        UC_ARM_REG_R7,
        UC_ARM_REG_R8,
        UC_ARM_REG_R13,
        UC_ARM_REG_R14,
        UC_ARM_REG_R15,
    )
except ImportError as error:
    raise SystemExit("the Python Unicorn bindings are required") from error


PAGE_SIZE = 0x1000
CODE_BASE = 0x00100000
SOURCE_BASE = 0x01000000
OUTPUT_BASE = 0x02000000
PREVIOUS_BASE = 0x02800000
STACK_BASE = 0x03000000
RETURN_ADDRESS = 0x04000000


def page_size(size: int) -> int:
    return (size + PAGE_SIZE - 1) & -PAGE_SIZE


def read_exact(path: Path, size: int, description: str) -> bytes:
    data = path.read_bytes()
    if len(data) != size:
        raise ValueError(
            f"{path}: {description} is {len(data)} bytes; expected {size}"
        )
    return data


def yuv_bytes_to_words(data: bytes) -> bytes:
    words = bytearray()
    for offset in range(0, len(data), 3):
        y, u, v = data[offset : offset + 3]
        if y > 63 or u > 31 or v > 31:
            raise ValueError(
                f"invalid 6Y5UV sample at pixel {offset // 3}: {y},{u},{v}"
            )
        words += struct.pack("<I", y | (u << 6) | (v << 11))
    return bytes(words)


def words_to_yuv_bytes(data: bytes) -> bytes:
    pixels = bytearray()
    for (word,) in struct.iter_unpack("<I", data):
        pixels += bytes((word & 0x3F, (word >> 6) & 0x1F,
                         (word >> 11) & 0x1F))
    return bytes(pixels)


def yuv555_words_to_6y5uv_bytes(data: bytes) -> bytes:
    """Convert unpatched type-7 YUV555 words to type-19 working samples."""
    pixels = bytearray()
    for (word,) in struct.iter_unpack("<I", data):
        y5 = word & 0x1F
        # CompLib's full-range component conversion rounds to nearest.
        y6 = (y5 * 63 + 15) // 31
        pixels += bytes((y6, (word >> 5) & 0x1F, (word >> 10) & 0x1F))
    return bytes(pixels)


def map_and_write(machine: Uc, address: int, data: bytes,
                  minimum_size: int = 0) -> None:
    mapped_size = page_size(max(len(data), minimum_size, 1))
    machine.mem_map(address, mapped_size, UC_PROT_ALL)
    if data:
        machine.mem_write(address, data)


def install_classic_ldm_lookahead(machine: Uc, decompressor: bytes,
                                  codec: int) -> None:
    """Emulate pre-ARMv6 alignment for Moving Blocks bitstream LDMs.

    Unicorn performs these LDMIA instructions from the literal unaligned byte
    address. Acorn ARM cores ignored the low address bits; the surrounding ARM
    shifts then selected the requested bit position within aligned words.
    """
    if codec == 23:
        instruction = bytes.fromhex("e00095e8")  # LDMIA r5,{r5,r6,r7}
        offsets = [
            offset for offset in range(len(decompressor))
            if decompressor.startswith(instruction, offset)
        ]
        if len(offsets) != 1:
            raise ValueError(
                "unexpected type-23 LDM signature count: "
                f"found {len(offsets)}, expected 1"
            )

        def emulate_type23(machine: Uc, current: int, size: int,
                           unused: object) -> None:
            source = machine.reg_read(UC_ARM_REG_R5)
            if source & 3:
                first, second, third = struct.unpack(
                    "<III", bytes(machine.mem_read(source & ~3, 12))
                )
                machine.reg_write(UC_ARM_REG_R5, first)
                machine.reg_write(UC_ARM_REG_R6, second)
                machine.reg_write(UC_ARM_REG_R7, third)
                machine.reg_write(UC_ARM_REG_R15, current + size)

        address = CODE_BASE + offsets[0]
        machine.hook_add(
            UC_HOOK_CODE, emulate_type23, None, address, address
        )
        return

    lookaheads = {
        # Format 19 has four header sites; format 7 has five decoder paths.
        bytes.fromhex("600097e8"): (4 if codec == 19 else 5, UC_ARM_REG_R7,
                                    UC_ARM_REG_R5, UC_ARM_REG_R6),
        # The two Huffman paths use r6 as both base and first destination.
        bytes.fromhex("400196e8"): (1, UC_ARM_REG_R6,
                                    UC_ARM_REG_R6, UC_ARM_REG_R8),
        bytes.fromhex("c00096e8"): (1, UC_ARM_REG_R6,
                                    UC_ARM_REG_R6, UC_ARM_REG_R7),
    }

    def emulate(machine: Uc, current: int, size: int,
                registers: tuple[int, int, int]) -> None:
        base_register, first_register, second_register = registers
        source = machine.reg_read(base_register)
        if source & 3:
            first, second = struct.unpack(
                "<II", bytes(machine.mem_read(source & ~3, 8))
            )
            machine.reg_write(first_register, first)
            machine.reg_write(second_register, second)
            machine.reg_write(UC_ARM_REG_R15, current + size)

    for instruction, description in lookaheads.items():
        expected_count, base_register, first_register, second_register = (
            description
        )
        if codec == 7 and expected_count == 1:
            expected_count = 0
        offsets = []
        offset = decompressor.find(instruction)
        while offset >= 0:
            offsets.append(offset)
            offset = decompressor.find(instruction, offset + 1)
        if len(offsets) != expected_count:
            raise ValueError(
                "unexpected Moving Blocks bitstream lookahead signature count: "
                f"found {len(offsets)}, expected {expected_count}"
            )
        for offset in offsets:
            address = CODE_BASE + offset
            machine.hook_add(
                UC_HOOK_CODE, emulate,
                (base_register, first_register, second_register),
                address, address
            )


def numbered_path(prefix: Path, frame: int, suffix: str) -> Path:
    return Path(f"{prefix}{frame:06d}{suffix}")


def run(args: argparse.Namespace) -> int:
    if args.width == 0 or args.height == 0 or args.width % 4 or args.height % 4:
        raise ValueError("dimensions must be positive multiples of four")

    pixel_count = args.width * args.height
    frame_bytes = pixel_count * 3
    arm_frame_bytes = pixel_count * 4
    decompressor = args.decompressor.read_bytes()
    payload = args.payload.read_bytes()
    if len(decompressor) < 12:
        raise ValueError("decompressor is too short to contain its header")
    if not payload:
        raise ValueError("payload is empty")
    if len(decompressor) > SOURCE_BASE - CODE_BASE:
        raise ValueError("decompressor is too large for the harness memory map")
    if len(payload) + 8 > OUTPUT_BASE - SOURCE_BASE:
        raise ValueError("payload is too large for the harness memory map")
    if arm_frame_bytes > PREVIOUS_BASE - OUTPUT_BASE:
        raise ValueError("frame is too large for the harness memory map")

    if args.previous_words16 is not None:
        previous_halfwords = read_exact(
            args.previous_words16, pixel_count * 2, "16-bit previous frame"
        )
        previous_words = b"".join(
            struct.pack("<I", value)
            for (value,) in struct.iter_unpack("<H", previous_halfwords)
        )
    elif args.previous is None:
        previous_words = bytes(arm_frame_bytes)
    else:
        previous_words = yuv_bytes_to_words(
            read_exact(args.previous, frame_bytes, "previous frame")
        )

    machine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    map_and_write(machine, CODE_BASE, decompressor)
    # The decoder performs word loads beyond the final meaningful payload byte.
    map_and_write(machine, SOURCE_BASE, payload, len(payload) + 8)
    map_and_write(machine, OUTPUT_BASE, b"", arm_frame_bytes)
    map_and_write(machine, PREVIOUS_BASE, previous_words)
    map_and_write(machine, STACK_BASE, b"", PAGE_SIZE)
    map_and_write(machine, RETURN_ADDRESS, struct.pack("<I", 0xE1A00000))
    install_classic_ldm_lookahead(machine, decompressor, args.codec)

    machine.reg_write(UC_ARM_REG_R0, args.width)
    machine.reg_write(UC_ARM_REG_R1, args.height)
    # CodecIf: any value other than "PARM" means no parameter list. This
    # decompressor ignores both registers, but zero states that intent clearly.
    machine.reg_write(UC_ARM_REG_R2, 0)
    machine.reg_write(UC_ARM_REG_R3, 0)
    machine.reg_write(UC_ARM_REG_R13, STACK_BASE + PAGE_SIZE)
    machine.reg_write(UC_ARM_REG_R14, RETURN_ADDRESS)
    machine.emu_start(CODE_BASE + 4, RETURN_ADDRESS)

    source_offset = 0
    for frame in range(args.frames):
        # Alternate buffers so the just-decoded reconstruction becomes the
        # immutable previous frame for the following CodecIf call.
        output_address = OUTPUT_BASE if frame % 2 == 0 else PREVIOUS_BASE
        previous_address = PREVIOUS_BASE if frame % 2 == 0 else OUTPUT_BASE
        machine.reg_write(UC_ARM_REG_R0, SOURCE_BASE + source_offset)
        machine.reg_write(UC_ARM_REG_R1, output_address)
        machine.reg_write(UC_ARM_REG_R2, previous_address)
        machine.reg_write(UC_ARM_REG_R3, 0)
        machine.reg_write(UC_ARM_REG_R4, RETURN_ADDRESS)
        machine.reg_write(UC_ARM_REG_R13, STACK_BASE + PAGE_SIZE)
        # The Info file's `,C` selects CodecIf's r14 C-call return convention.
        # Also set r4 because the generator's older interface comment names it.
        machine.reg_write(UC_ARM_REG_R14, RETURN_ADDRESS)
        machine.emu_start(CODE_BASE + 8, RETURN_ADDRESS)

        next_source = machine.reg_read(UC_ARM_REG_R0)
        consumed = next_source - SOURCE_BASE
        if consumed < source_offset or consumed > len(payload):
            raise ValueError(
                "decompressor returned invalid source consumption for "
                f"frame {frame}: {consumed} after {source_offset}"
            )
        output_words = bytes(machine.mem_read(output_address, arm_frame_bytes))
        output_yuv = (
            yuv555_words_to_6y5uv_bytes(output_words)
            if args.output_layout == "yuv555-to-6y5uv"
            else words_to_yuv_bytes(output_words)
        )
        if args.output is not None:
            output_path = args.output
        else:
            output_path = numbered_path(args.output_prefix, frame, ".6y5uv")
        output_path.write_bytes(output_yuv)
        if args.output_words_prefix is not None:
            numbered_path(args.output_words_prefix, frame, ".words").write_bytes(
                output_words
            )
        if args.payload_prefix is not None:
            numbered_path(args.payload_prefix, frame, ".mb19").write_bytes(
                payload[source_offset:consumed]
            )
        print(
            f"frame={frame} codec={args.codec} source_start={source_offset} "
            f"source_end={consumed} bytes={consumed - source_offset} "
            f'output="{output_path}" status=ok'
        )
        source_offset = consumed

    print(
        f'decompressor="{args.decompressor}" payload="{args.payload}" '
        f"codec={args.codec} size={args.width}x{args.height} "
        f"frames={args.frames} "
        f"consumed={source_offset} trailing={len(payload) - source_offset} "
        "status=ok"
    )
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decompressor", required=True, type=Path)
    parser.add_argument("--codec", type=int, choices=(7, 19, 23), default=19)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--size", required=True)
    previous_group = parser.add_mutually_exclusive_group()
    previous_group.add_argument("--previous", type=Path)
    previous_group.add_argument(
        "--previous-words16", type=Path,
        help="native little-endian 16-bit key image expanded to ARM words",
    )
    output_group = parser.add_mutually_exclusive_group(required=True)
    output_group.add_argument("--output", type=Path)
    output_group.add_argument("--output-prefix", type=Path)
    parser.add_argument("--payload-prefix", type=Path)
    parser.add_argument("--output-words-prefix", type=Path)
    parser.add_argument(
        "--output-layout", choices=("6y5uv", "yuv555-to-6y5uv"),
        default="6y5uv",
        help="interpret output words directly or convert type-7 YUV555",
    )
    parser.add_argument("--frames", type=int, default=1)
    args = parser.parse_args()
    try:
        width, height = (int(value) for value in args.size.lower().split("x", 1))
    except ValueError as error:
        parser.error("--size must be WIDTHxHEIGHT")
        raise AssertionError from error
    args.width = width
    args.height = height
    if args.frames <= 0:
        parser.error("--frames must be positive")
    if args.frames != 1 and args.output is not None:
        parser.error("--output can only be used with --frames 1")
    return args


if __name__ == "__main__":
    try:
        sys.exit(run(parse_args()))
    except (OSError, ValueError, UcError) as error:
        raise SystemExit(f"decomp19-unicorn: {error}") from error
