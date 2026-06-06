# Format-19 Encoder Policy Comparison

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
codes vary with vector family, while a format-19 spatial code occupies the
shared radius-three family. A selected temporal vector may therefore be either
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

Until original-compressor fixtures are available, the expected directions
above are qualitative. No numerical bitrate or quality advantage is claimed.
