# Moving Blocks Decision Comparison

## What Can Be Compared Without The Source

The encoded type 19 (Super Moving Blocks) stream completely determines block
mode, motion vector, semantic bit range, reconstruction, and stored payload
size. These are valid observations even when the compressor's source pixels
are unavailable.

For chunk 0 of `LionFish19,ae7`, compiled Decomp19 and the portable verifier
agree on 25 frames consuming 181,885 bytes. The catalogue video size is
181,886 bytes, so the final byte is alignment padding.

The chunk contains 32,000 top-level 4x4 positions. Acorn selected:

| Mode | Count | Share |
| --- | ---: | ---: |
| Data | 3,394 | 10.6% |
| Stationary | 428 | 1.3% |
| Temporal | 7,832 | 24.5% |
| Spatial | 1,507 | 4.7% |
| Split | 18,839 | 58.9% |

The 75,356 children of split blocks contain 13,061 data, 1,460 stationary,
49,507 temporal, and 11,328 spatial 2x2 blocks. Temporal copies account for
65.7% of split children. This establishes that 2x2 temporal matching is a
primary compression mechanism in this sample.

## Confirmed Source And Conversion Trap

The source is confirmed as `LionFish2,ae7`, type 7 (Moving Blocks), 160x128 at
12.5 fps. `LionFishT2,ae7` captures the exact uncompressed words used for the
comparison, but its `[6Y5UV]` declaration is wrong.

CompLib only runs `-Convert 6Y5UV` when the source has a `Dec24` decoder. Type 7
does not, so YUV555 halfwords are copied unchanged and merely labelled 6Y5UV.
Replay consequently displays blocks with incorrect colour. Chunk 0 confirms
the payload is YUV555: bit 15 is never set and all three 5-bit fields use their
full range.

The payload remains useful as the authoritative compressor input. With the
same words interpreted as type 19 fields, Acorn's 25 chunk-0 reconstructions
measure 45.221729 dB luma PSNR and maximum luma error 2. This close match
confirms frame alignment and replaces the earlier, incorrect conclusion that
the source movie might differ.

The corrected playable intermediate is `LionFishX,ae7`, type 2 (16 bit colour
uncompressed) with `[YUV]` in the pixel-depth field. It is 16,118,852 bytes and
has SHA-256
`f6a71e4e73dda589d131146ae0de79f4e350fbdcd2fe7bed891e3a39b1b41020`.
Its chunk-0 source metrics against `LionFish19` are identical to the incorrectly
labelled intermediate, confirming that only the declaration changed.

## Comparison Tools

`replay-verify --summary` emits hierarchical mode counts and non-overlapping
per-mode bit totals. Split containers contribute their two header bits; their
children contribute their own complete codes. The totals reconcile to the
semantic bit count.

`replay-encode --input-format 6y5uv` accepts packed native samples directly.
This avoids RGB conversion when an Acorn source reconstruction is available.

`tools/mb19_compare_reports.py` sums bytes, bits, mode counts, squared error,
and sample counts across frames. Sequence PSNR is calculated from total squared
error; frame PSNR values are not averaged.

## Source-Matched Result

`LionFishSMB,ae7` independently confirms the reference encode. It is a type 19
(Super Moving Blocks) movie whose video and audio payloads are byte-identical
to `LionFish19,ae7`; only container metadata differs. Its SHA-256 is
`544e953436deaefeb4499854963495e11e4471f1bdd1eb8709da4aa2b9f18520`.

At quality 7, encoding the same first 25 source frames from `LionFishX,ae7`
produces 181,220 bytes with the portable encoder, compared with Acorn's 181,885
bytes. The portable stream is 0.366% smaller, but its luma PSNR is 42.765507 dB
instead of Acorn's 45.221729 dB, a loss of 2.456 dB. Both have maximum luma
error 2.

The portable ordered policy selects 2,071 stationary 4x4 blocks versus Acorn's
428, and only 545 spatial 4x4 blocks versus Acorn's 1,507. At 2x2 it selects
5,737 stationary and 3,279 spatial blocks, versus Acorn's 1,460 and 11,328.
This supports replacing family priority with a measured cross-family
lowest-error choice. Emitted bits and stable table order should be explicit
tie-breakers. Acorn parity can remain a later selectable policy rather than the
default implementation target.

That policy is now implemented for 4x4 blocks and split 2x2 children. On the
same 25 frames it emits 179,656 bytes, 1.225% below Acorn, and reaches
45.236548 dB luma PSNR, 0.015 dB above Acorn. U and V PSNR are 19.258155 dB
and 27.268218 dB, respectively 0.240 dB and 0.462 dB below Acorn. Compared
with the earlier ordered policy it saves 1,564 bytes and gains 2.471 dB luma
PSNR. It is now the portable command-line default; `--policy ordered` retains
the old behavior for controlled comparisons.

The comparison also found that a split 2x2 spatial vector can geometrically
reach a future top-level 4x4 parent. Such pixels have not yet been
reconstructed, so the reference is invalid. The portable encoder and verifier
now enforce this reconstruction-order rule.

## Quality Sweep

A five-point sweep over loss levels 0, 7, 14, 21, and 28 confirms that the
level-7 result is not isolated. Lowest-error is both smaller and higher quality
than ordered through level 14. At level 21 it emits 122,808 bytes versus
119,139 while retaining 34.220987 dB versus 33.310493 dB luma PSNR. At level
28 it emits 99,500 bytes versus 93,382 while retaining 29.508717 dB versus
28.626563 dB.

Thus lowest-error clearly makes better local reconstruction choices, but at
high loss levels it may spend more bits preserving that quality. The next fair
comparison is at matched target bytes, where each policy's rate control may
select a different loss level. The sweep is reproducible with
`tools/mb19_quality_sweep.py`; it preserves payloads, decoded frames, traces,
and verifier reports for every point.

The sweep driver also supports target-byte runs. A first real-frame attempt
showed that adjacent quality-row retries can repeat many complete temporal and
spatial searches. The portable tool now offers adjacent-first exponential
bracketing followed by binary refinement, while retaining linear search for
compatibility comparisons. Every probe remains independently decoded.

At a 6,000-byte inter-frame target over the same 25 frames, lowest-error emits
149,184 total bytes at 40.663672 dB luma PSNR. Ordered emits 150,004 bytes at
38.965991 dB. The totals include the common 9,875-byte key frame. Thus the
default policy is 820 bytes smaller and 1.698 dB better in luma at this matched
target, with U and V improvements of 0.897 dB and 1.204 dB respectively.

Bracketed and linear search produced byte-identical final payloads in a
five-frame comparison. Candidate-score reuse is still the next performance
step for broad full-movie target matrices.

`replay-extract` now recovers all 375 fixed-size frames from `LionFishX,ae7`.
Its `type19-fields` layout deliberately reinterprets stored halfwords as the
fields supplied to the historical type 19 compressor. Frames 0 through 24 are
byte-identical to the previously validated source corpus.
