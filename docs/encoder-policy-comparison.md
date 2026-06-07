# Type 19, Super Moving Blocks Encoder Policy Comparison

Bitstream compatibility does not require reproducing every decision made by
Acorn's compressor. The useful requirement is to identify policy differences
and measure their effect on payload size and decoder-visible reconstruction.

## Common Ground

Both compressors use the same format constraints:

- raster-order 4x4 blocks, optionally split into four 2x2 blocks;
- stationary and temporal copies from the previous reconstructed frame;
- spatial copies from pixels already reconstructed in the current frame;
- data blocks with block chroma and Huffman-coded luma residuals;
- the 29-row `maxi`, `maxe`, and total-error acceptance table;
- reconstructed output, rather than source pixels, as the next reference.

Consequently, a policy difference changes which legal reconstruction is
chosen. It does not change how that reconstruction is decoded.

## Acorn Policy

`BatchComp` searches previous-frame and current-frame candidates while keeping
`answer%`, documented in the source as the best match distance so far. An
accepted 4x4 copy bypasses literal coding. If there is no accepted 4x4 copy,
the compressor constructs the four 2x2 decisions, constructs a 4x4 data block,
and keeps the split only when its emitted bit count is no greater.

The search therefore gives reconstruction error a stronger role across motion
families than the portable encoder currently does. Source-order tie behavior
also matters because later candidates can replace an equal-distance result.

## Portable Policy

The portable encoder currently applies this deterministic order:

1. accepted stationary copy;
2. lowest-error accepted temporal copy;
3. lowest-error accepted spatial copy;
4. 4x4 data versus split, with 4x4 data winning an equal-bit tie.

Temporal and spatial searches preserve table order when errors are equal. The
policy is deliberately simple and fully documented; Acorn decision parity is
not an implementation goal.

## Expected Consequences

Stationary priority normally minimizes bits because its 4x4 code is only two
bits. At lossy quality levels it can, however, retain a higher-error stationary
block when Acorn's broader search would find a closer temporal or spatial
match. This can reduce bitrate at the cost of local reconstruction quality.

Temporal priority over spatial has no fixed bitrate direction. Temporal motion
codes vary with vector family, while a type 19, Super Moving Blocks spatial
code occupies the shared radius-three family. A selected temporal vector may therefore be either
shorter or longer than an available spatial vector. It may also have greater
error because the portable encoder does not compare the winning temporal and
spatial errors with each other.

These choices can affect later frames as well as the current block. A changed
reconstruction becomes a temporal reference, and a changed spatial block can
affect later blocks in the same frame. Per-block error alone therefore does not
fully predict sequence quality or bitrate.

The portable encoder's strict `<` split test differs from Acorn's apparent
keep-split-on-equal behavior. This does not change the current payload size in
an equal-bit case, but it can change reconstructed pixels and thus later
decisions.

## Required Measurements

A useful comparison corpus must run identical quantised source frames and
quality levels through both compressors and record, per block:

- mode and motion vector;
- emitted bits;
- decoder-visible luma and chroma error;
- reconstructed-frame checksum;
- following-frame mode and size changes.

Sequence summaries should report total bytes, mode counts, mean squared error,
PSNR in the codec's 6Y5UV domain, and maximum component error. RGB metrics may
be added for presentation, but 6Y5UV is the authoritative comparison because
that is what the codec actually preserves.

The portable tooling emits these block traces and reports native 6Y5UV
SSE/MSE, PSNR, and maximum component errors. `LionFishX,ae7` supplies the
authoritative type 2 source words seen by both compressors.

## Source-Matched Portable Comparison

`LionFishSMB,ae7` is an independently generated type 19 (Super Moving Blocks)
reference. Its video and audio payloads are byte-identical to `LionFish19,ae7`;
only container header and sprite-area metadata differ. Its SHA-256 is
`544e953436deaefeb4499854963495e11e4471f1bdd1eb8709da4aa2b9f18520`.

Encoding the same first 25 source frames from `LionFishX,ae7` at quality 7
gives:

| Encoder | Payload bytes | Y PSNR | U PSNR | V PSNR | Maximum Y error |
| --- | ---: | ---: | ---: | ---: | ---: |
| Acorn | 181,885 | 45.221729 dB | 19.498089 dB | 27.730550 dB | 2 |
| Portable ordered | 181,220 | 42.765507 dB | 18.689158 dB | 27.071052 dB | 2 |
| Portable lowest-error | 179,656 | 45.236548 dB | 19.258155 dB | 27.268218 dB | 2 |

The portable ordered result is 665 bytes, or 0.366%, smaller, but its luma
PSNR is 2.456 dB lower. This is the expected consequence of its simple ordered
policy, not evidence of a better rate-distortion result. It accepts stationary
before considering other families and does not compare the winning temporal
and spatial errors against each other.

The cross-family lowest-error policy removes that regression. It is 2,229
bytes, or 1.225%, smaller than Acorn while its luma PSNR is 0.015 dB higher.
Relative to ordered it saves 1,564 bytes and gains 2.471 dB luma PSNR. Its
chroma PSNR remains below Acorn by 0.240 dB U and 0.462 dB V, so this one chunk
does not establish universal rate-distortion superiority.

The mode counts make that policy difference visible:

| Mode | Acorn | Portable |
| --- | ---: | ---: |
| Data 4x4 | 3,394 | 3,521 |
| Stationary 4x4 | 428 | 2,071 |
| Temporal 4x4 | 7,832 | 6,820 |
| Spatial 4x4 | 1,507 | 545 |
| Split 4x4 | 18,839 | 19,043 |
| Data 2x2 | 13,061 | 13,675 |
| Stationary 2x2 | 1,460 | 5,737 |
| Temporal 2x2 | 49,507 | 53,481 |
| Spatial 2x2 | 11,328 | 3,279 |

With lowest-error, the corresponding counts are 3,412 data, 686 stationary,
7,916 temporal, 1,004 spatial, and 18,982 split top-level blocks. Split
children comprise 13,242 data, 2,638 stationary, 53,428 temporal, and 6,620
spatial blocks. This moves the family distribution substantially toward
Acorn's decisions without attempting exact decision parity.

The earlier ordered encoder therefore over-selects stationary modes and
substantially under-selects spatial modes. `lowest-error` is now implemented
for both 4x4 blocks and split 2x2 children, using emitted bits and stable
family/table order as explicit tie-breakers. It is the command-line default;
`--policy ordered` retains the previous behavior for comparison.

This run also exposed an availability rule that must be enforced independently
of policy. A split 2x2 spatial vector may point upward and right into a future
top-level 4x4 block. Those pixels have not been reconstructed and are not a
legal reference. The encoder now rejects such candidates, and the verifier
rejects such streams instead of reading stale destination-buffer contents.

## Fixed-Level Sweep

The first 25 authoritative `LionFishX,ae7` frames were also encoded at five
points across the 29-row quality table:

| Policy | Loss level | Bytes | Y PSNR | U PSNR | V PSNR |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lowest-error | 0 | 225,081 | infinite | 18.033461 dB | 26.094036 dB |
| Lowest-error | 7 | 179,656 | 45.236548 dB | 19.258155 dB | 27.268218 dB |
| Lowest-error | 14 | 157,197 | 41.990885 dB | 19.898674 dB | 29.641578 dB |
| Lowest-error | 21 | 122,808 | 34.220987 dB | 17.987300 dB | 29.264060 dB |
| Lowest-error | 28 | 99,500 | 29.508717 dB | 16.424236 dB | 26.686726 dB |
| Ordered | 0 | 226,326 | infinite | 17.999226 dB | 26.084338 dB |
| Ordered | 7 | 181,220 | 42.765507 dB | 18.689158 dB | 27.071052 dB |
| Ordered | 14 | 157,944 | 40.077905 dB | 18.914767 dB | 28.802220 dB |
| Ordered | 21 | 119,139 | 33.310493 dB | 17.535410 dB | 28.547800 dB |
| Ordered | 28 | 93,382 | 28.626563 dB | 15.798252 dB | 26.181533 dB |

At levels 0, 7, and 14, lowest-error is both smaller and higher quality. At
levels 21 and 28 it retains 0.911 dB and 0.882 dB more luma PSNR but emits
3.1% and 6.6% more bytes. Equal loss-level rows therefore do not establish a
complete rate-distortion ordering. The next comparison should sweep target
bytes, allowing rate control to choose a loss level for each policy.

Infinite level-0 luma PSNR means every decoded luma sample matches the source.
Chroma is not lossless because data blocks store one averaged U/V pair per
block even when copy matching itself requires exact decoder-visible values.

## Original Chunk-0 Decision Profile

The first chunk of `LionFish19,ae7`, produced by Acorn's type 19 (Super Moving
Blocks) compressor, provides a source-independent view of its decisions:

```text
frames                 25
decoder-consumed bytes 181885
semantic bits          1454985
average bytes/frame    7275.4
```

Across 32,000 top-level 4x4 positions, Acorn selected 3,394 data, 428
stationary, 7,832 temporal, 1,507 spatial, and 18,839 split blocks. Splits are
58.9% of top-level positions. Their 75,356 child blocks comprise 13,061 data,
1,460 stationary, 49,507 temporal, and 11,328 spatial blocks; temporal copies
are 65.7% of split children.

Per-mode semantic bit totals are:

```text
data 4x4          331901
stationary 4x4       856
temporal 4x4       71945
spatial 4x4        13563
split headers      37678
data 2x2          475859
stationary 2x2      2920
temporal 2x2      429639
spatial 2x2        90624
```

These sum exactly to the decoded semantic-bit total. They show that split
selection and temporal 2x2 matching are central to Acorn's output, not rare
fallbacks. This profile is suitable for structural regression tests even
before source-referenced quality comparison is available.

The source-referenced chunk-0 Acorn reconstruction has 45.221729 dB luma PSNR
and maximum luma error 2 against the type 2 intermediate. U and V metrics need
careful interpretation because the historical path carries YUV555 words into
the type 19 field layout rather than performing the requested component
conversion.
