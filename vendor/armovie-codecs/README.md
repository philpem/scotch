# Acorn Replay (ARMovie) decompressor modules

These are the original Acorn Replay / ARMovie compiled video `Decompress`
modules, used by this project as **byte-exact cross-check oracles**: the
portable C codecs and the `replay-armsim` harness are validated against the
genuine Acorn decoders run under the vendored ARMulator (see
`../armulator`).

| Directory | Replay codec | Format |
|-----------|-------------:|--------|
| `MovingLine/` | 1  | Moving Lines |
| `Decomp7/`    | 7  | Moving Blocks |
| `Decomp17/`   | 17 | Moving Blocks HQ |
| `Decomp19/`   | 19 | Super Moving Blocks |
| `Decomp20/`   | 20 | Moving Blocks Beta |
| `Decomp20new/`| 20 | Moving Blocks Beta (v0.05, delta chroma) |
| `Decomp23/`   | 23 | 6Y6Y5U5V |

Each `Decompress,ffd` is the compiled ARM module (RISC OS filetype `&FFD`),
run unpatched by the harness so it emits the codec's native working-colour
words.

## Licensing and acknowledgement

The Acorn Replay codec modules were distributed by Acorn as **freeware** and are
now **open source via RISC OS Open Ltd**. They are included here under that
provenance, with thanks to RISC OS Open and the original Acorn authors.

- RISC OS Open: <https://gitlab.riscosopen.org/>
- The ARMovie / Replay sources live in the RISC OS system resources
  (`RiscOS/Sources/SystemRes/ARMovie`), from which these decompressors are
  built.

They are reproduced here unmodified, solely as decode-compatibility test
fixtures.
