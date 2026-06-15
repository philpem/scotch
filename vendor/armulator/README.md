# Vendored ARMulator

This directory contains a trimmed copy of the ARM instruction emulator
("ARMulator") from the GNU debugger's ARM simulator, used by the
`replay-armsim` tool and the `codecif` library to run the original compiled
Acorn Replay decompressors.

## License and acknowledgement

The vendored sources are part of **GDB / GNU Binutils** and were taken from the
`sim/arm` directory of the **binutils-gdb 16.3** release (plus
`include/ansidecl.h` from the same tree). They originate as ARM Ltd's ARMulator
("Copyright (C) 1994 Advanced RISC Machines Ltd.") and carry later
contributions by Cygnus/Red Hat and the FSF.

This code is licensed under the **GNU General Public License, version 3 or
later** — see `COPYING` in this directory for the full text. The original GPL
notices remain intact at the top of each vendored `.c`/`.h` file. The local glue
and shim files described below are likewise GPLv3-or-later as derivative works.

Upstream project: <https://sourceware.org/git/binutils-gdb.git>, `sim/arm`.

## Why this core

The Replay decompressors rely on classic Acorn ARM memory semantics that later
cores (and Unicorn, which models one) do not reproduce:

- an unaligned `LDR` rotates the loaded word by the byte offset, and
- an unaligned `LDM`/word load ignores the low two address bits.

This ARMulator models both directly (`ARMul_Align` for the rotate; `GetWord`
masking the low bits for the multiple/word loads), so the per-codec
instruction-signature alignment shims the Unicorn Python harnesses needed are
unnecessary. See `docs/decomp19-arm-harness.md`.

## Provenance

Source: `gdb-16.3/sim/arm` from the binutils-gdb 16.3 release, plus
`include/ansidecl.h` from the same tree. Upstream is licensed GPL (the files
carry "Copyright (C) 1994 Advanced RISC Machines Ltd." and GPL headers).

Files taken verbatim from `sim/arm`:

| file | role |
|------|------|
| `armemu.c`   | main ARM instruction emulation (compiled twice; see below) |
| `armemu32.c` | `#include "armemu.c"` with `MODE32` to build the 32-bit core |
| `armsupp.c`  | CPSR/mode helpers, coprocessor dispatch, `ARMul_Align`, events |
| `arminit.c`  | state creation, processor selection, reset, run loop |
| `armvirt.c`  | flat, paged "virtual memory" model (the swappable memory layer) |
| `armdefs.h`, `armemu.h`, `armos.h`, `dbg_rdi.h`, `iwmmxt.h` | headers |
| `ansidecl.h` | from `gdb-16.3/include` (attribute macros) |

## Local additions / shims (replay-tooling, not upstream)

- `armsim_harness.c` — implements the public `replay/armsim.h` API on top of
  the emulator and supplies the host-environment symbols the GDB simulator
  framework would normally provide (`stop_simulator`, `trace`, `disas`,
  `print_insn`, the `XScale_*`/`read_cp15_reg` helpers, `ARMul_ConsolePrint`,
  `ARMul_CoProInit`/`Exit`, `ARMul_HandleIwmmxt`, `ARMul_ThumbDecode`, and the
  `ARMul_OSHandleSWI` return sentinel). All of those resolve runtime-dead paths
  — no coprocessor, Thumb, XScale or OS-SWI code is executed by the codecs.
- `defs.h` — minimal stand-in for the GDB sim's generated `defs.h` (the vendored
  files use no `HAVE_`/`WITH_` config macros). Also provides a portable
  `ATTRIBUTE_FALLTHROUGH` fallback, which this `ansidecl.h` predates.
- `libiberty.h` — one-line stand-in providing only `ARRAY_SIZE` (the sole symbol
  `armsupp.c` uses from libiberty).

## Modifications to vendored files

- `armvirt.c`: pages are allocated with `calloc` instead of `malloc` so the
  simulated address space reads back as zero before being written, matching the
  Unicorn harness this replaces. (Two one-line changes, marked with comments.)

## Build notes

- `MODET` must be defined: `armemu.c`'s shared `donext` label is only compiled
  under `MODET`, even though no Thumb is executed.
- Compiled with warnings relaxed (`-w`) and `-fno-strict-aliasing` (the page
  table is accessed through aggressive type punning). The harness/CodecIf layers
  on top are built with the project's normal strict warnings.
