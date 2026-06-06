#!/usr/bin/env python3
"""Run Acorn's generated format-19 decompressor under Unicorn.

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


def map_and_write(machine: Uc, address: int, data: bytes,
                  minimum_size: int = 0) -> None:
    mapped_size = page_size(max(len(data), minimum_size, 1))
    machine.mem_map(address, mapped_size, UC_PROT_ALL)
    if data:
        machine.mem_write(address, data)


def install_classic_ldm_lookahead(machine: Uc, decompressor: bytes) -> None:
    """Emulate pre-ARMv6 alignment for Decomp19's two bitstream LDMs.

    Unicorn performs these LDMIA instructions from the literal unaligned byte
    address. Acorn ARM cores ignored the low address bits; the following ARM
    shifts then selected the requested bit position within the aligned words.
    """
    lookaheads = {
        bytes.fromhex("400196e8"): UC_ARM_REG_R8,  # LDMIA r6,{r6,r8}
        bytes.fromhex("c00096e8"): UC_ARM_REG_R7,  # LDMIA r6,{r6,r7}
    }
    for instruction, second_register in lookaheads.items():
        offset = decompressor.find(instruction)
        if offset < 0:
            raise ValueError("Decomp19 Huffman lookahead instruction not found")
        if decompressor.find(instruction, offset + 1) >= 0:
            raise ValueError("Decomp19 Huffman lookahead signature is ambiguous")
        address = CODE_BASE + offset

        def emulate(machine: Uc, current: int, size: int, user_data: int) -> None:
            source = machine.reg_read(UC_ARM_REG_R6)
            if source & 3:
                first, second = struct.unpack(
                    "<II", bytes(machine.mem_read(source & ~3, 8))
                )
                machine.reg_write(UC_ARM_REG_R6, first)
                machine.reg_write(user_data, second)
                machine.reg_write(UC_ARM_REG_R15, current + size)

        machine.hook_add(
            UC_HOOK_CODE, emulate, second_register, address, address
        )


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

    if args.previous is None:
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
    install_classic_ldm_lookahead(machine, decompressor)

    machine.reg_write(UC_ARM_REG_R0, args.width)
    machine.reg_write(UC_ARM_REG_R1, args.height)
    # CodecIf: any value other than "PARM" means no parameter list. This
    # decompressor ignores both registers, but zero states that intent clearly.
    machine.reg_write(UC_ARM_REG_R2, 0)
    machine.reg_write(UC_ARM_REG_R3, 0)
    machine.reg_write(UC_ARM_REG_R13, STACK_BASE + PAGE_SIZE)
    machine.reg_write(UC_ARM_REG_R14, RETURN_ADDRESS)
    machine.emu_start(CODE_BASE + 4, RETURN_ADDRESS)

    machine.reg_write(UC_ARM_REG_R0, SOURCE_BASE)
    machine.reg_write(UC_ARM_REG_R1, OUTPUT_BASE)
    machine.reg_write(UC_ARM_REG_R2, PREVIOUS_BASE)
    machine.reg_write(UC_ARM_REG_R3, 0)
    machine.reg_write(UC_ARM_REG_R4, RETURN_ADDRESS)
    machine.reg_write(UC_ARM_REG_R13, STACK_BASE + PAGE_SIZE)
    # The Info file's `,C` selects CodecIf's r14 C-call return convention.
    # Also set r4 because the generator's older interface comment names it.
    machine.reg_write(UC_ARM_REG_R14, RETURN_ADDRESS)
    machine.emu_start(CODE_BASE + 8, RETURN_ADDRESS)

    next_source = machine.reg_read(UC_ARM_REG_R0)
    consumed = next_source - SOURCE_BASE
    if consumed < 0 or consumed > len(payload):
        raise ValueError(
            f"decompressor returned invalid source consumption: {consumed}"
        )
    output_words = bytes(machine.mem_read(OUTPUT_BASE, arm_frame_bytes))
    args.output.write_bytes(words_to_yuv_bytes(output_words))
    print(
        f'decompressor="{args.decompressor}" payload="{args.payload}" '
        f"size={args.width}x{args.height} consumed={consumed} "
        f'output="{args.output}" status=ok'
    )
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decompressor", required=True, type=Path)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--size", required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        width, height = (int(value) for value in args.size.lower().split("x", 1))
    except ValueError as error:
        parser.error("--size must be WIDTHxHEIGHT")
        raise AssertionError from error
    args.width = width
    args.height = height
    return args


if __name__ == "__main__":
    try:
        sys.exit(run(parse_args()))
    except (OSError, ValueError, UcError) as error:
        raise SystemExit(f"decomp19-unicorn: {error}") from error
