#!/usr/bin/env python3
"""Run the compiled Acorn Moving Lines (type 1) decompressor under Unicorn.

The bundled module is !ARMovie_compiled/MovingLine/Decompress (the word-per-pixel
variant). Left unpatched, it stores each decoded pixel as its raw 15-bit value in
an ARM word, so the harness reads those words back and writes them as native
little-endian 16-bit halfwords -- the same representation this project's
codec_movinglines uses. This lets the portable codec be cross-checked
byte-for-byte against the genuine module (see tests/test_movinglines_compiled.sh).

The module follows the standard ARMovie CodecIf contract: a three-word header
(patch-table offset, init branch, decompress branch); init builds the copy offset
table; decompress decodes precisely one frame, advancing r0 (the source pointer)
to the next frame and taking r1=output, r2=previous reconstruction.
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
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
        UC_ARM_REG_R11, UC_ARM_REG_R13, UC_ARM_REG_R14, UC_ARM_REG_R15,
    )
except ImportError as error:
    raise SystemExit("the Python Unicorn bindings are required") from error

PAGE = 0x1000
CODE = 0x00100000
SOURCE = 0x01000000
OUTPUT = 0x02000000
PREVIOUS = 0x02800000
STACK = 0x03000000
RETURN = 0x04000000


def page_round(size: int) -> int:
    return (size + PAGE - 1) & -PAGE


def map_write(machine: Uc, address: int, data: bytes, minimum: int = 0) -> None:
    machine.mem_map(address, page_round(max(len(data), minimum, 1)), UC_PROT_ALL)
    if data:
        machine.mem_write(address, data)


def halfwords_to_words(data: bytes) -> bytes:
    """Expand native little-endian 16-bit pixels to one ARM word each."""
    return b"".join(struct.pack("<I", v) for (v,) in struct.iter_unpack("<H", data))


def install_decn_alignment(machine: Uc, module: bytes) -> None:
    """Emulate pre-ARMv6 alignment for the long-literal block load.

    The long-literal (0x1f) path reads its bit-packed pixels with `LDMIA r7,...`
    where r7 is a byte address into the packed data and is usually not
    word-aligned: the group path (dcn2) loads three words into {r8,r9,r11}, the
    remainder path (dcn1) loads two into {r8,r9}. Acorn ARM cores read LDM from
    the word-aligned address (low bits ignored), and the surrounding `r0 & 31`
    shift then selects the bit position. Unicorn instead loads from the literal
    unaligned address, so emulate the aligned load for both.
    """
    # LDMIA r7,{r8,r9,r11} (3 words) and LDMIA r7,{r8,r9} (2 words).
    loads = {
        bytes.fromhex("000b97e8"): (UC_ARM_REG_R8, UC_ARM_REG_R9,
                                    UC_ARM_REG_R11),
        bytes.fromhex("000397e8"): (UC_ARM_REG_R8, UC_ARM_REG_R9),
    }

    def make_emulator(destinations: tuple[int, ...]):
        def emulate(uc: Uc, address: int, size: int, user: object) -> None:
            base = uc.reg_read(UC_ARM_REG_R7)
            if base & 3:
                words = struct.unpack(
                    "<" + "I" * len(destinations),
                    bytes(uc.mem_read(base & ~3, 4 * len(destinations))),
                )
                for register, value in zip(destinations, words):
                    uc.reg_write(register, value)
                uc.reg_write(UC_ARM_REG_R15, address + size)
        return emulate

    for signature, destinations in loads.items():
        offsets = [i for i in range(len(module))
                   if module.startswith(signature, i)]
        if len(offsets) != 1:
            raise ValueError(
                f"unexpected long-literal LDM signature count: {len(offsets)}"
            )
        address = CODE + offsets[0]
        machine.hook_add(UC_HOOK_CODE, make_emulator(destinations), None,
                         address, address)


def run(args: argparse.Namespace) -> int:
    width, height = args.width, args.height
    pixels = width * height
    module = args.decompressor.read_bytes()
    payload = args.payload.read_bytes()
    if len(module) < 12:
        raise ValueError("decompressor is too short to contain its header")
    if args.previous is not None:
        previous = halfwords_to_words(
            Path(args.previous).read_bytes()[: pixels * 2]
        )
    else:
        previous = bytes(pixels * 4)

    machine = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    map_write(machine, CODE, module)
    map_write(machine, SOURCE, payload, len(payload) + 16)
    map_write(machine, OUTPUT, b"", pixels * 4)
    map_write(machine, PREVIOUS, previous, pixels * 4)
    map_write(machine, STACK, b"", PAGE)
    map_write(machine, RETURN, struct.pack("<I", 0xE1A00000))  # the return target
    install_decn_alignment(machine, module)

    # init: build the offset table (r0/r1 carry the frame dimensions).
    for reg, value in ((UC_ARM_REG_R0, width), (UC_ARM_REG_R1, height),
                       (UC_ARM_REG_R2, 0), (UC_ARM_REG_R3, 0),
                       (UC_ARM_REG_R13, STACK + PAGE), (UC_ARM_REG_R14, RETURN)):
        machine.reg_write(reg, value)
    machine.emu_start(CODE + 4, RETURN)

    # decompress one frame. r3 (the colour lookup) stays 0: an unpatched module
    # stores the raw pixel word, which is exactly what we want to read back.
    for reg, value in ((UC_ARM_REG_R0, SOURCE), (UC_ARM_REG_R1, OUTPUT),
                       (UC_ARM_REG_R2, PREVIOUS), (UC_ARM_REG_R3, 0),
                       (UC_ARM_REG_R4, RETURN), (UC_ARM_REG_R13, STACK + PAGE),
                       (UC_ARM_REG_R14, RETURN)):
        machine.reg_write(reg, value)
    machine.emu_start(CODE + 8, RETURN)

    consumed = machine.reg_read(UC_ARM_REG_R0) - SOURCE
    if consumed < 0 or consumed > len(payload):
        raise ValueError(f"module consumed {consumed} of {len(payload)} bytes")
    words = struct.iter_unpack("<I", bytes(machine.mem_read(OUTPUT, pixels * 4)))
    args.output.write_bytes(b"".join(struct.pack("<H", w & 0xFFFF) for (w,) in words))
    print(f"codec=1 size={width}x{height} consumed={consumed} "
          f"trailing={len(payload) - consumed} status=ok")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decompressor", required=True, type=Path)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--size", required=True)
    parser.add_argument("--previous", type=Path,
                        help="previous frame as native 16-bit pixels")
    parser.add_argument("--output", required=True, type=Path,
                        help="decoded frame, written as native 16-bit pixels")
    args = parser.parse_args()
    try:
        width, height = (int(v) for v in args.size.lower().split("x", 1))
    except ValueError as error:
        parser.error("--size must be WIDTHxHEIGHT")
        raise AssertionError from error
    if width <= 0 or height <= 0:
        parser.error("--size must be positive")
    args.width, args.height = width, height
    return args


if __name__ == "__main__":
    try:
        sys.exit(run(parse_args()))
    except (OSError, ValueError, UcError) as error:
        raise SystemExit(f"movinglines-unicorn: {error}") from error
