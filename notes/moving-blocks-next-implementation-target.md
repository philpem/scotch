# Moving Blocks Implementation Target

The best first portable C target is format 19, `Super Moving Blocks`.

## Why Not Format 7 First

Format 7 is simpler and already well specified, so it remains useful for a
decoder/verifier and bit-writer smoke test. However, it is the oldest format:

- 5-bit luma;
- smaller motion search;
- no explicit stationary top-level case;
- fixed raw data costs.

It is a good learning format, but less representative of the later Moving
Blocks family.

## Why Not Format 17 First

Format 17 is now understood well enough to implement, but it is a halfway
point:

- same newer block syntax as 19/20;
- 5-bit YUV;
- 32-symbol luma Huffman residual table.

It is useful as the bridge from the documented stream file to the real
Huffman-coded implementation, but format 19 is a better practical target.

## Why Format 19 First

Format 19 keeps the useful later structure while avoiding Beta's chroma
predictor complication:

- HQ block and motion syntax;
- explicit stationary blocks;
- 8-pixel motion search;
- 6-bit luma;
- 5-bit literal chroma;
- 64-symbol luma Huffman table;
- no `D4tab`;
- no `prevu`/`prevv` bitstream state.

That means a C encoder can focus on the core Moving Blocks decisions:

- reconstructed-frame feedback;
- temporal and spatial searches;
- 4x4 versus 2x2 decisions;
- luma prediction and Huffman residual coding;
- quality and bitrate retry behaviour.

## Suggested Build Order

1. Implement shared bitstream writer, least-significant-bit first.
2. Implement format-19 data-coded 4x4 and 2x2 blocks only.
3. Implement a small format-19 decoder or decoder-side verifier for those data
   blocks.
4. Add stationary and temporal same-position copies.
5. Add full motion/spatial tables.
6. Add the `QP%` match thresholds and block search.
7. Add the retry loop and byte-budget control.
8. Only then consider format 20's chroma delta state.

The highest-risk implementation detail is not the Huffman table itself. It is
state synchronisation: every trial encode must reconstruct exactly what the
decoder will reconstruct, including corrected Huffman residual outputs and
predictor state.
