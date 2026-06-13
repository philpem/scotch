# C Tooling Verification Strategy

This note describes how to verify the portable Replay compressor as it is
built. Verification should come before compression quality work. A stream that
is small but subtly desynchronises decoder state is worse than a large
data-only stream that can be explained and decoded exactly.

## Verification Goals

The tooling needs to prove several different things:

- primitive writers produce the intended bytes;
- compressed payloads are syntactically valid;
- decoded output matches the encoder's reconstructed frame;
- temporal and spatial references use decoder-visible reconstructed pixels;
- generated payloads are accepted by the original Replay decompressor;
- final Replay files are accepted by Player, not just by local tests.

No single verifier proves all of that. The project should use several
independent checks, each aimed at a different failure class.

## Verification Layers

Use this ladder:

```text
unit tests
  -> codec-payload verifier
      -> encoder/reconstruction self-check
          -> source-derived golden cases
              -> original Acorn decompressor check
                  -> Replay player check
```

The early layers should run constantly. The later layers can be manual or
integration tests until the surrounding tooling exists.

## Layer 1: Primitive Unit Tests

These tests catch mistakes before any codec logic is involved.

Test `ReplayBuffer`:

- reserve and append;
- capacity growth;
- clear without freeing;
- free after partial construction;
- allocation failure handling where practical.

Test `ReplayBitWriter`:

- writing 1, 2, 5, 8, 12, and 17 bits;
- writes that cross byte boundaries;
- least-significant-bit-first order;
- zero flush of partial bytes;
- reported bit position;
- reset and reuse.

Test `ReplayBitReader`:

- read back every writer test;
- detect truncated fields;
- reject reads beyond end of payload.

Test halfword helpers:

- little-endian write/read;
- odd payload length rejection;
- truncated Moving Lines command rejection.

These tests should not mention Moving Blocks or Moving Lines. They verify the
tooling substrate.

## Layer 2: Format-19 Symbol Tests

Before encoding whole blocks, test the fixed format-19 luma Huffman table.

For each residual symbol `0..63`:

1. write the symbol with the table-driven Huffman writer;
2. decode it with the verifier-side Huffman reader;
3. assert that the decoded residual is identical;
4. assert that the number of written bits matches the documented table.

This is not fully independent, because both sides use C code. It still catches
bit-order, table-length, and prefix-walk mistakes. The documented table in
`notes/moving-blocks-format19-bitstream.md` should be treated as the review
source for these tests.

## Layer 3: Data-Block Round Trips

The first codec verifier should support only data-coded blocks:

- format-19 4x4 data blocks;
- format-19 2x2 data blocks;
- luma prediction update;
- literal 5-bit U/V reconstruction.

Tests should use hand-built synthetic blocks:

- all-zero luma residuals;
- maximum luma residuals;
- wraparound residual cases;
- alternating chroma;
- a block whose final predictor value is easy to calculate manually.

For each block:

1. encode the block;
2. decode the payload with the local verifier;
3. compare decoded pixels with the encoder reconstruction;
4. compare final luma predictor state.

The comparison target is the reconstructed frame, not the original source
frame. Lossy encoding means the source and decoded frame are not expected to be
identical after quantisation and candidate selection.

## Layer 4: Full-Frame Payload Verification

Once data-coded blocks work, verify complete frame payloads.

Start with data-only frames:

- one 4x4 block;
- two adjacent 4x4 blocks;
- one full scanline of blocks;
- small synthetic frame, such as 16x16;
- real-sized frame, such as 320x256.

The verifier should check:

- the payload consumes the expected number of bits/bytes;
- no trailing required data is missing;
- block scan position reaches exactly the frame end;
- decoded frame equals encoder reconstruction;
- per-frame predictor state resets exactly when expected.

After that, add modes one at a time:

- stationary copy;
- temporal move/copy;
- spatial move/copy;
- split 2x2.

Do not add rate control until a fixed-loss encode attempt is verifier-clean.

## Layer 5: Encoder Self-Check Mode

The encoder should have a development mode that runs the verifier immediately
after each encoded frame:

```text
replay-enc ... --verify-payload
```

For each frame, this should:

1. encode the frame;
2. decode the generated payload through the local verifier;
3. compare verifier output with the encoder's reconstructed frame;
4. report the first mismatching pixel, block, and mode trace if any.

This catches the most dangerous class of compressor bug: the encoder makes
later decisions using a reconstructed frame that differs from what the decoder
will actually have.

Useful failure output:

```text
verify: frame=12 block=44,28 pixel=46,30 component=y encoder=37 decoder=36
verify: previous block trace: frame=12 block=44,28 mode=data4x4 bits=71
```

The self-check should be available for raw payloads before Replay container
writing exists.

## Layer 6: Source-Derived Golden Cases

Golden tests should be small byte sequences whose expected behaviour is derived
from the original source and the notes.

Good early golden cases:

- exact bytes for simple bit-writer sequences;
- exact bytes for a known format-19 4x4 data block;
- exact bytes for a known format-19 2x2 data block;
- exact reconstructed pixels for a stationary block using a known previous
  frame;
- exact reconstructed pixels for one temporal offset;
- exact reconstructed pixels for one spatial offset.

Golden cases are valuable because they stop the local writer and local reader
from drifting together. They should be few, explicit, and reviewed against the
documented bitstream notes.

## Layer 7: Original Acorn Decompressor Cross-Check

The stronger oracle is the original Replay decompressor.

For format 19, the relevant source is:

- `ARMovie_2003/Resources/Documents/CodecIf`, the authoritative general
  decompressor file and register interface;
- `ARMovie_2003/Video/Decomp19/bas/MakeDecomp,ffb.txt`
- generated/runtime code under `ARMovie_2003/Video/Decomp19`
- duplicated 2003 system-resource paths under
  `RiscOS_2003/RiscOS/Sources/SystemRes/ARMovie/Video/Decomp19`

The target check is:

1. generate a frame payload from the C encoder;
2. feed it to the original decompressor path, or to a harness built from it;
3. capture decoded pixels;
4. compare those pixels with the C verifier output.

The compiled decompressor is present under
`!ARMovie_compiled/Decomp19/Decompress,ffd` and now runs under Unicorn through
`replay-tooling/tools/decomp19_unicorn.py`. The harness follows `CodecIf`,
converts unpatched output words to packed `Y,U,V`, and emulates classic ARM
alignment for four block-header and two Huffman lookahead `LDMIA` operations.

The local verifier remains useful even after this exists, because it is fast
and can produce better diagnostics. The original decompressor is the
compatibility oracle.

### Practical ARM Harness Split

Two independent reference paths are useful and test different things:

1. Compress a tiny uncompressed movie with the original format-19 compressor
   in a RISC OS emulator. Preserve the input RGB bytes, dimensions, compressor
   settings, complete movie, and extracted frame payloads.
2. Run the format-19 `Decompress` routine in Unicorn. This is implemented and
   maps input payload, previous frame, output frame, code, and stack directly.

The first path checks original encoder policy and container extraction. The
second is now a fast decoder oracle in CTest. Focused stationary, temporal,
spatial, split, and lossy fixtures pass, including temporal and spatial 2x2
blocks.

The portable tooling now defines the handoff format used by both paths. A raw
decoded frame is exactly `width * height * 3` bytes in raster order, with one
`Y,U,V` triplet per pixel. There is no header or row padding, and five-bit
chroma remains in modulo form rather than being sign-extended. The
`replay-verify` options are:

```text
--previous-6y5uv FILE   previous reconstructed frame for inter payloads
--output-6y5uv FILE     portable decoder output
--expect-6y5uv FILE     Acorn output to compare byte-for-byte
```

Corpus metadata lives in `replay-tooling/corpus/format19/manifest.tsv`. Each
entry records dimensions, payload, optional previous frame, expected Acorn
output, and provenance. `tools/check_format19_corpus.sh` runs every row and
reports the first differing pixel and component triplets. The manifest must
distinguish emulator-produced originals from payloads generated by the C
encoder; the latter are useful ARM decoder tests but are not original-encoder
compatibility evidence.

The verifier can now emit a per-block trace containing mode, motion vector,
and exact bit range. It can also compare decoded output against a source 6Y5UV
frame using signed five-bit chroma differences and report SSE, MSE, PSNR, and
maximum component error. The encoder records the same metrics for every retry.
This is the basis for comparing the portable policy with Acorn's compressor
without requiring identical decisions.

## Layer 8: Replay Container And Player Verification

A payload can be valid while the movie file is not. Container verification
should be separate.

Check the Replay writer in stages:

- parse back the written container and validate chunk sizes/offsets;
- extract each compressed payload and run the local codec verifier;
- verify compression type, dimensions, frame rate, and key-frame metadata;
- play the file with an existing Replay player.

The first playable file should use conservative choices:

- video only;
- format 19;
- fixed dimensions;
- low frame count;
- data-only or mostly data-coded payloads;
- simple frame rate such as 12.5 fps.

Once that plays, compression improvements can be tested against the same
container path.

## Moving Lines Verification

Moving Lines should get a separate verifier because its stream structure is
different.

Verifier support should cover:

- literal 15-bit pixels;
- temporal/spatial copy commands;
- same-position previous-frame copy commands;
- packed literal runs;
- repeated-pixel runs;
- end-of-frame marker;
- byte, halfword, and word output only after the word-output verifier is
  correct.

Moving Lines should share:

- `ReplayBuffer`;
- halfword helpers;
- frame comparison utilities;
- CLI verifier conventions.

It should not share the Moving Blocks bit writer or block verifier.

## What Counts As Passing

For a codec payload:

- the verifier consumes exactly the payload needed for one frame;
- decoded pixels match the encoder reconstructed frame;
- previous-frame state after decode matches the encoder's stored previous
  frame for the next frame;
- malformed or truncated payloads fail cleanly.

For an encoder mode:

- at least one focused golden or synthetic test covers it;
- self-check passes on synthetic frames;
- self-check passes on at least one real raw-video sequence;
- trace output identifies the mode and its reference source.

For a Replay file:

- the local container parser can extract every frame;
- every extracted payload passes codec verification;
- an existing Replay player or decompressor accepts the file.

## Recommended Next Implementation Step

Build verification scaffolding before the encoder:

1. `ReplayBuffer`;
2. `ReplayBitWriter`;
3. `ReplayBitReader`;
4. unit tests for bit order;
5. format-19 Huffman decode table;
6. tests for all 64 format-19 residual symbols;
7. one manually reviewed 4x4 data-block golden case.

Only after that should the first encoder function write a data-coded block.
